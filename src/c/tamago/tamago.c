/*
 * tamago.c - top-level emulator integration
 *
 * Provides the public API declared in tamago.h.
 */

#include "tamago_internal.h"

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
uint8_t   g_rom_bank_buf[TAMAGO_ROM_BANK_SIZE];
uint8_t   g_static_rom[TAMAGO_STATIC_ROM_SIZE];
uint16_t  g_irq_vectors[16];
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
  memset(&g_cpu, 0, sizeof(g_cpu));
  memset(g_wram, 0, sizeof(g_wram));
  memset(g_dram, 0, sizeof(g_dram));
  memset(g_cpureg, 0, sizeof(g_cpureg));
  g_keys = 0xFF;
  g_rom_bank_id = 0xFF;
  g_rom_loaded = false;

  if (!tamago_load_static_rom()) return false;
  tamago_set_rom_bank(0);
  tamago_cpu_reset();

  g_initialised = true;
  APP_LOG(APP_LOG_LEVEL_INFO,
          "tamago_init: ready, PC=$%04x", g_cpu.pc);
  return true;
}

void tamago_release(void)
{
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

uint32_t tamago_step_cycles(uint32_t target_cycles)
{
  if (!g_initialised) return 0;

  tamago_fire_irq(10);
  tamago_fire_nmi(6);

  uint32_t spent = 0;
  while (spent < target_cycles) {
    spent += tamago_cpu_step();
  }

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
