/*
 * tamago.c - top-level emulator integration
 *
 * Provides the public API declared in tamago.h.
 */

#include "tamago_internal.h"
#include "tamago_eeprom.h"

// ----- Global state definitions ------------------------------------------
//
// Declared extern in tamago_internal.h. Living as plain globals (rather
// than fields of a g_sys struct) lets the compiler resolve addresses at
// compile time and skip a pointer-indirection on every hot-path memory
// access. ~52 KB total, all in .bss.

cpu6502_t g_cpu;
uint8_t   g_wram[TAMAGO_WRAM_SIZE];
uint8_t   g_dram[TAMAGO_DRAM_SIZE];
uint8_t   g_cpureg[TAMAGO_CPUREG_SIZE];
uint8_t  *g_rom_bank_buf = NULL;   // malloc'd in tamago_init
uint8_t  *g_static_rom   = NULL;   // malloc'd in tamago_init
uint16_t  g_irq_vectors[16];
uint8_t   g_irq_pending_any;
uint8_t   g_rom_bank_id;
bool      g_rom_loaded;
uint8_t   g_keys;
ResHandle g_rom_resource;
bool      g_initialised;

// ----- Init / Release -----------------------------------------------------

bool tamago_init(void)
{
  if (g_initialised) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "tamago_init: already initialised");
    return true;
  }

  // Allocate the big ROM caches on the heap. They have to live somewhere
  // and putting them in .bss pushes us over Pebble's 16-bit virtual-size
  // limit (65535 bytes). The heap on Emery has plenty of room.
  g_rom_bank_buf = (uint8_t *)malloc(TAMAGO_ROM_BANK_SIZE);
  g_static_rom   = (uint8_t *)malloc(TAMAGO_STATIC_ROM_SIZE);
  if (!g_rom_bank_buf || !g_static_rom) {
    APP_LOG(APP_LOG_LEVEL_ERROR,
            "tamago_init: out of memory (bank=%p static=%p)",
            g_rom_bank_buf, g_static_rom);
    if (g_rom_bank_buf) { free(g_rom_bank_buf); g_rom_bank_buf = NULL; }
    if (g_static_rom)   { free(g_static_rom);   g_static_rom   = NULL; }
    return false;
  }

  memset(&g_cpu, 0, sizeof(g_cpu));
  memset(g_wram, 0, sizeof(g_wram));
  memset(g_dram, 0, sizeof(g_dram));
  memset(g_cpureg, 0, sizeof(g_cpureg));
  memset(g_rom_bank_buf, 0, TAMAGO_ROM_BANK_SIZE);
  memset(g_static_rom,   0, TAMAGO_STATIC_ROM_SIZE);
  g_keys = 0xFF;
  g_rom_bank_id = 0xFF;
  g_rom_loaded = false;

  if (!tamago_load_static_rom()) return false;
  tamago_set_rom_bank(0);
  tamago_eeprom_init();      // load save data from persist
  tamago_cpu_reset();

  g_initialised = true;
  APP_LOG(APP_LOG_LEVEL_INFO,
          "tamago_init: ready, PC=$%04x", g_cpu.pc);
  return true;
}

void tamago_release(void)
{
  // Flush any pending EEPROM writes back to persist before we tear down.
  // Without this, save data written between the last STOP-after-WRITE
  // and app exit would be lost.
  tamago_eeprom_flush();
  if (g_rom_bank_buf) { free(g_rom_bank_buf); g_rom_bank_buf = NULL; }
  if (g_static_rom)   { free(g_static_rom);   g_static_rom   = NULL; }
  g_initialised = false;
}

void tamago_reset(void)
{
  if (!g_initialised) return;
  tamago_cpu_reset();
}

// ----- Stepping -----------------------------------------------------------

uint8_t tamago_step_one(void)
{
  if (!g_initialised) return 0;
  return tamago_cpu_step();
}

static uint32_t s_tbh_accumulator = 0;
#define TBH_RATE (TAMAGO_CLOCK_RATE / 2)

// "Video" IRQ/NMI fire rate.
//
// On real Tama-Go hardware these are driven by the LCD controller, which
// runs at roughly 60 Hz. We step the emulator 20× per second, so we need
// to fire 3 sub-frames worth of NMI/IRQ per call to match the rate the
// ROM expects.
//
// Symptoms when this is wrong: the Tama's clock runs slow because the
// game counts video frames to measure real time (e.g. "60 frames = 1
// second"). At 20 Hz it would clock at 1/3 speed.
#define VIDEO_FIRES_PER_FRAME 3

uint32_t tamago_step_cycles(uint32_t target_cycles)
{
  if (!g_initialised) return 0;

  // Split the requested cycles into VIDEO_FIRES_PER_FRAME chunks so the
  // ROM sees the video interrupt at roughly the hardware rate.
  uint32_t per_chunk = target_cycles / VIDEO_FIRES_PER_FRAME;
  uint32_t spent = 0;

  for (int chunk = 0; chunk < VIDEO_FIRES_PER_FRAME; chunk++) {
    tamago_fire_irq(10);
    tamago_fire_nmi(6);

    uint32_t chunk_target = (chunk == VIDEO_FIRES_PER_FRAME - 1)
                              ? target_cycles   // last chunk takes the rest
                              : (spent + per_chunk);
    while (spent < chunk_target) {
      spent += tamago_cpu_step();
    }
  }

  // 2 Hz timer (IRQ 13) — the "TBH" counter that drives the game's
  // real-time clock. Independent of the video IRQ rate.
  s_tbh_accumulator += spent;
  while (s_tbh_accumulator >= TBH_RATE) {
    tamago_fire_irq(13);
    s_tbh_accumulator -= TBH_RATE;
  }
  return spent;
}

// ----- Display ------------------------------------------------------------

static const uint16_t LCD_ROW_OFFSETS[31] = {
  0x0C0, 0x0CC, 0x0D8, 0x0E4,
  0x0F0, 0x0FC, 0x108, 0x114,
  0x120, 0x12C, 0x138, 0x144,
  0x150, 0x15C, 0x168, 0x174,
  0x0B4, 0x0A8, 0x09C, 0x090,
  0x084, 0x078, 0x06C, 0x060,
  0x054, 0x048, 0x03C, 0x030,
  0x024, 0x018, 0x00C,
};

void tamago_get_display(uint8_t *out)
{
  if (!g_initialised || !out) return;
  for (int y = 0; y < TAMAGO_LCD_HEIGHT; y++) {
    uint16_t base = LCD_ROW_OFFSETS[y] % TAMAGO_DRAM_SIZE;
    for (int x = 0; x < TAMAGO_LCD_WIDTH; x += 4) {
      uint8_t d = g_dram[base++];
      out[y * TAMAGO_LCD_WIDTH + x + 0] = (d >> 6) & 0x3;
      out[y * TAMAGO_LCD_WIDTH + x + 1] = (d >> 4) & 0x3;
      out[y * TAMAGO_LCD_WIDTH + x + 2] = (d >> 2) & 0x3;
      out[y * TAMAGO_LCD_WIDTH + x + 3] = (d >> 0) & 0x3;
    }
  }
}

// Icon extraction matches the refresh_simple loop in tamago/main.js:
//   var a=4, b=0, g=0;
//   while (g<10) { glyph = (dram[a] >> b) & 3; if ((b-=2)<0){b=6;a++;} ... }
//
// Walking the loop manually gives the table below. Two bits per icon,
// four icons per byte, 10 icons total across 4 bytes (DRAM[4..7]) — but
// only the first 2 bits of DRAM[4] and first 2 bits of DRAM[7] are used.
void tamago_get_icons(uint8_t *out)
{
  if (!g_initialised || !out) return;
  out[0] = (g_dram[4] >> 0) & 0x3;   // dashboard
  out[1] = (g_dram[5] >> 6) & 0x3;   // food
  out[2] = (g_dram[5] >> 4) & 0x3;   // trash
  out[3] = (g_dram[5] >> 2) & 0x3;   // globe
  out[4] = (g_dram[5] >> 0) & 0x3;   // user
  out[5] = (g_dram[6] >> 6) & 0x3;   // comments
  out[6] = (g_dram[6] >> 4) & 0x3;   // medkit
  out[7] = (g_dram[6] >> 2) & 0x3;   // heart
  out[8] = (g_dram[6] >> 0) & 0x3;   // book
  out[9] = (g_dram[7] >> 6) & 0x3;   // bell
}

// ----- Buttons ------------------------------------------------------------

void tamago_set_buttons(uint8_t mask)
{
  if (!g_initialised) return;
  uint8_t lo = ~mask & 0x0F;
  g_keys = lo | 0xF0;
}

// ----- Debug --------------------------------------------------------------

void tamago_debug_get_state(uint16_t *pc, uint8_t *a, uint8_t *x, uint8_t *y,
                            uint8_t *s, uint8_t *bank)
{
  if (!g_initialised) return;
  if (pc)   *pc   = g_cpu.pc;
  if (a)    *a    = g_cpu.a;
  if (x)    *x    = g_cpu.x;
  if (y)    *y    = g_cpu.y;
  if (s)    *s    = g_cpu.s;
  if (bank) *bank = g_rom_bank_id;
}

bool tamago_debug_dram_dirty(void)
{
  if (!g_initialised) return false;
  for (int i = 0; i < TAMAGO_DRAM_SIZE; i++) if (g_dram[i]) return true;
  return false;
}

uint16_t tamago_debug_dram_nonzero_count(void)
{
  if (!g_initialised) return 0;
  uint16_t n = 0;
  for (int i = 0; i < TAMAGO_DRAM_SIZE; i++) if (g_dram[i]) n++;
  return n;
}

// ----- Save state ---------------------------------------------------------

#define TAMAGO_SAVE_MAGIC   0x54414D47
#define TAMAGO_SAVE_VERSION 1
#define TAMAGO_SAVE_SIZE                                                     \
  (4 + 4 + sizeof(cpu6502_t) +                                               \
   TAMAGO_CPUREG_SIZE + TAMAGO_WRAM_SIZE + TAMAGO_DRAM_SIZE + 1 + 1)

uint32_t tamago_serialize_state(uint8_t *buf, uint32_t bufsize)
{
  if (!buf || bufsize < TAMAGO_SAVE_SIZE) return TAMAGO_SAVE_SIZE;
  if (!g_initialised) return 0;
  uint8_t *p = buf;
  uint32_t magic = TAMAGO_SAVE_MAGIC;
  memcpy(p, &magic, 4); p += 4;
  *p++ = TAMAGO_SAVE_VERSION;
  *p++ = 0; *p++ = 0; *p++ = 0;
  memcpy(p, &g_cpu, sizeof(cpu6502_t)); p += sizeof(cpu6502_t);
  memcpy(p, g_cpureg, TAMAGO_CPUREG_SIZE); p += TAMAGO_CPUREG_SIZE;
  memcpy(p, g_wram, TAMAGO_WRAM_SIZE);     p += TAMAGO_WRAM_SIZE;
  memcpy(p, g_dram, TAMAGO_DRAM_SIZE);     p += TAMAGO_DRAM_SIZE;
  *p++ = g_rom_bank_id;
  *p++ = g_keys;
  return (uint32_t)(p - buf);
}

bool tamago_deserialize_state(const uint8_t *buf, uint32_t bufsize)
{
  if (!buf || bufsize < TAMAGO_SAVE_SIZE || !g_initialised) return false;
  const uint8_t *p = buf;
  uint32_t magic;
  memcpy(&magic, p, 4); p += 4;
  if (magic != TAMAGO_SAVE_MAGIC) return false;
  uint8_t version = *p++;
  if (version != TAMAGO_SAVE_VERSION) return false;
  p += 3;
  memcpy(&g_cpu, p, sizeof(cpu6502_t)); p += sizeof(cpu6502_t);
  memcpy(g_cpureg, p, TAMAGO_CPUREG_SIZE); p += TAMAGO_CPUREG_SIZE;
  memcpy(g_wram, p, TAMAGO_WRAM_SIZE);     p += TAMAGO_WRAM_SIZE;
  memcpy(g_dram, p, TAMAGO_DRAM_SIZE);     p += TAMAGO_DRAM_SIZE;
  uint8_t saved_bank = *p++;
  g_keys = *p++;
  tamago_set_rom_bank(saved_bank);
  return true;
}
