/*
 * tamago.c - top-level emulator integration
 *
 * Provides the public API declared in tamago.h:
 *   - tamago_init / tamago_release
 *   - tamago_reset
 *   - tamago_step_cycles / tamago_step_one
 *   - tamago_get_display
 *   - tamago_set_buttons
 *   - tamago_serialize_state / tamago_deserialize_state
 */

#include "tamago_internal.h"

// Global system pointer. Declared extern in tamago_internal.h.
tamago_system_t *g_sys = NULL;

// ----- Init / Release -----------------------------------------------------

bool tamago_init(void)
{
  if (g_sys) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "tamago_init: already initialised");
    return true;
  }

  g_sys = (tamago_system_t *)malloc(sizeof(tamago_system_t));
  if (!g_sys) {
    APP_LOG(APP_LOG_LEVEL_ERROR,
            "tamago_init: malloc failed (%u bytes)",
            (unsigned)sizeof(tamago_system_t));
    return false;
  }
  memset(g_sys, 0, sizeof(tamago_system_t));

  // Buttons in idle (pull-up) state — bit set = not pressed.
  g_sys->keys = 0xFF;

  // Mark "no bank loaded yet" with a sentinel value so the first
  // tamago_set_rom_bank() call doesn't short-circuit.
  g_sys->rom_bank_id = 0xFF;

  // Load the static (high) ROM and build the IRQ vector table.
  if (!tamago_load_static_rom()) {
    free(g_sys);
    g_sys = NULL;
    return false;
  }

  // Load bank 0 by default; the ROM's reset code may switch immediately.
  tamago_set_rom_bank(0);

  // Reset CPU (jumps to the reset vector at $FFFC).
  tamago_cpu_reset();

  APP_LOG(APP_LOG_LEVEL_INFO,
          "tamago_init: ready, PC=$%04x, heap used ~%u bytes",
          g_sys->cpu.pc, (unsigned)sizeof(tamago_system_t));
  return true;
}

void tamago_release(void)
{
  if (!g_sys) return;
  free(g_sys);
  g_sys = NULL;
}

void tamago_reset(void)
{
  if (!g_sys) return;
  tamago_cpu_reset();
}

// ----- Stepping -----------------------------------------------------------

uint8_t tamago_step_one(void)
{
  if (!g_sys) return 0;
  return tamago_cpu_step();
}

// Counter for periodic IRQ/NMI fires. The JS emulator does these in
// step_realtime() — once per call (~per frame). We mirror that by firing
// them once per tamago_step_cycles call.
//
// The exact rates aren't documented in the JS source ("HACK" comments):
//   fire_irq(13)  — every CLOCK_RATE/2 cycles (2 Hz). This is the "TBH"
//                   timer that drives in-game time / animation tick.
//   fire_irq(10)  — once per refresh call (effectively video frame rate)
//   fire_nmi(6)   — once per refresh call (frame interrupt)
static uint32_t s_tbh_accumulator = 0;
#define TBH_RATE (TAMAGO_CLOCK_RATE / 2)   // 2 Hz

uint32_t tamago_step_cycles(uint32_t target_cycles)
{
  if (!g_sys) return 0;

  // Fire the per-frame "video" IRQ and NMI before stepping. The ROM uses
  // these to drive its main loop and screen refresh.
  tamago_fire_irq(10);
  tamago_fire_nmi(6);

  uint32_t spent = 0;
  while (spent < target_cycles) {
    spent += tamago_cpu_step();
  }

  // 2 Hz timer: accumulate cycles, fire IRQ(13) when we cross a TBH_RATE
  // boundary. This drives the game's slow timer (heartbeat, etc).
  s_tbh_accumulator += spent;
  while (s_tbh_accumulator >= TBH_RATE) {
    tamago_fire_irq(13);
    s_tbh_accumulator -= TBH_RATE;
  }
  return spent;
}

// ----- Display ------------------------------------------------------------
//
// The DRAM stores 2 bits per pixel, packed 4 pixels per byte. The order
// of bytes within DRAM is given by LCD_ORDER in the JS code — the
// hardware's scan order is asymmetric and skips a 32-byte block in the
// middle. We mirror that here.

// LCD scan order (from tamagotchi.js's LCD_ORDER array).
// Each entry is the DRAM offset for one row of 16 bytes (= 64 pixels).
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
  if (!g_sys || !out) return;
  // Walk 31 rows × 64 columns. Each byte holds 4 pixels (2 bits each).
  // Row 32 of the hardware buffer is unused/duplicate — we output 31
  // rows. Caller can either render 31 rows directly or pad row 32 to a
  // copy of row 31.
  for (int y = 0; y < 31; y++) {
    uint16_t base = LCD_ROW_OFFSETS[y] % TAMAGO_DRAM_SIZE;
    // Wrap-protect: in the original, DRAM is mirrored, so offsets
    // larger than 0x200 wrap. Mod ensures we don't read past the buffer.
    for (int x = 0; x < 64; x += 4) {
      uint8_t d = g_sys->dram[base++];
      // The JS extracts 4 pixels in order, MSB-pair first: bits 7-6, 5-4, 3-2, 1-0.
      out[y * TAMAGO_LCD_WIDTH + x + 0] = (d >> 6) & 0x3;
      out[y * TAMAGO_LCD_WIDTH + x + 1] = (d >> 4) & 0x3;
      out[y * TAMAGO_LCD_WIDTH + x + 2] = (d >> 2) & 0x3;
      out[y * TAMAGO_LCD_WIDTH + x + 3] = (d >> 0) & 0x3;
    }
  }
  // Last row (y=31): we just duplicate row 30 so the caller can render a
  // full 32-row buffer without seeing garbage. The hardware actually has
  // 32 commons but the JS emulator only renders 31. To stay safe.
  memcpy(out + 31 * TAMAGO_LCD_WIDTH,
         out + 30 * TAMAGO_LCD_WIDTH,
         TAMAGO_LCD_WIDTH);
}

// ----- Buttons ------------------------------------------------------------

void tamago_set_buttons(uint8_t mask)
{
  if (!g_sys) return;
  // External convention: bit set = pressed.
  // Internal convention: bit set = NOT pressed (pull-up).
  // The hardware has 4 buttons mapped to the low 4 bits.
  uint8_t lo = ~mask & 0x0F;
  // Keep the upper 4 bits at their pull-up default (always 1).
  g_sys->keys = lo | 0xF0;
}

// ----- Debug ---------------------------------------------------------------

void tamago_debug_get_state(uint16_t *pc, uint8_t *a, uint8_t *x, uint8_t *y,
                            uint8_t *s, uint8_t *bank)
{
  if (!g_sys) return;
  if (pc)   *pc   = g_sys->cpu.pc;
  if (a)    *a    = g_sys->cpu.a;
  if (x)    *x    = g_sys->cpu.x;
  if (y)    *y    = g_sys->cpu.y;
  if (s)    *s    = g_sys->cpu.s;
  if (bank) *bank = g_sys->rom_bank_id;
}

bool tamago_debug_dram_dirty(void)
{
  if (!g_sys) return false;
  for (int i = 0; i < TAMAGO_DRAM_SIZE; i++) {
    if (g_sys->dram[i]) return true;
  }
  return false;
}

uint16_t tamago_debug_dram_nonzero_count(void)
{
  if (!g_sys) return 0;
  uint16_t n = 0;
  for (int i = 0; i < TAMAGO_DRAM_SIZE; i++) {
    if (g_sys->dram[i]) n++;
  }
  return n;
}

// ----- Save state ---------------------------------------------------------

// Serialised state layout:
//   uint32_t magic      ("TAMG")
//   uint8_t  version    (current = 1)
//   uint8_t  reserved[3]
//   cpu6502_t cpu
//   uint8_t  cpureg[256]
//   uint8_t  wram[1536]
//   uint8_t  dram[512]
//   uint8_t  rom_bank_id
//   uint8_t  keys
// EEPROM is NOT serialised here — it lives in Pebble persist storage
// permanently.
#define TAMAGO_SAVE_MAGIC   0x54414D47  // "TAMG"
#define TAMAGO_SAVE_VERSION 1

#define TAMAGO_SAVE_SIZE                                                     \
  (4 + 4 +                                                                   \
   sizeof(cpu6502_t) +                                                       \
   TAMAGO_CPUREG_SIZE +                                                      \
   TAMAGO_WRAM_SIZE +                                                        \
   TAMAGO_DRAM_SIZE +                                                        \
   1 + 1)

uint32_t tamago_serialize_state(uint8_t *buf, uint32_t bufsize)
{
  if (!buf || bufsize < TAMAGO_SAVE_SIZE) return TAMAGO_SAVE_SIZE;
  if (!g_sys) return 0;

  uint8_t *p = buf;
  uint32_t magic = TAMAGO_SAVE_MAGIC;
  memcpy(p, &magic, 4); p += 4;
  *p++ = TAMAGO_SAVE_VERSION;
  *p++ = 0; *p++ = 0; *p++ = 0;
  memcpy(p, &g_sys->cpu, sizeof(cpu6502_t)); p += sizeof(cpu6502_t);
  memcpy(p, g_sys->cpureg, TAMAGO_CPUREG_SIZE); p += TAMAGO_CPUREG_SIZE;
  memcpy(p, g_sys->wram, TAMAGO_WRAM_SIZE);     p += TAMAGO_WRAM_SIZE;
  memcpy(p, g_sys->dram, TAMAGO_DRAM_SIZE);     p += TAMAGO_DRAM_SIZE;
  *p++ = g_sys->rom_bank_id;
  *p++ = g_sys->keys;
  return (uint32_t)(p - buf);
}

bool tamago_deserialize_state(const uint8_t *buf, uint32_t bufsize)
{
  if (!buf || bufsize < TAMAGO_SAVE_SIZE || !g_sys) return false;

  const uint8_t *p = buf;
  uint32_t magic;
  memcpy(&magic, p, 4); p += 4;
  if (magic != TAMAGO_SAVE_MAGIC) {
    APP_LOG(APP_LOG_LEVEL_WARNING,
            "tamago_deserialize: bad magic 0x%08lx", (unsigned long)magic);
    return false;
  }
  uint8_t version = *p++;
  if (version != TAMAGO_SAVE_VERSION) {
    APP_LOG(APP_LOG_LEVEL_WARNING,
            "tamago_deserialize: version %u not supported", version);
    return false;
  }
  p += 3;  // reserved
  memcpy(&g_sys->cpu, p, sizeof(cpu6502_t)); p += sizeof(cpu6502_t);
  memcpy(g_sys->cpureg, p, TAMAGO_CPUREG_SIZE); p += TAMAGO_CPUREG_SIZE;
  memcpy(g_sys->wram, p, TAMAGO_WRAM_SIZE);     p += TAMAGO_WRAM_SIZE;
  memcpy(g_sys->dram, p, TAMAGO_DRAM_SIZE);     p += TAMAGO_DRAM_SIZE;
  uint8_t saved_bank = *p++;
  g_sys->keys = *p++;

  // Reload the active ROM bank so the bank cache matches what the CPU
  // expects.
  tamago_set_rom_bank(saved_bank);

  return true;
}
