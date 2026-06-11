/*
 * tamago_internal.h - shared state structure for the emulator modules
 *
 * Not exposed to the Pebble app — only included by tamago_*.c files.
 */

#ifndef TAMAGO_INTERNAL_H
#define TAMAGO_INTERNAL_H

#include "tamago.h"

// 6502 registers
typedef struct {
  uint16_t pc;   // Program counter
  uint8_t  a;    // Accumulator
  uint8_t  x;    // Index X
  uint8_t  y;    // Index Y
  uint8_t  s;    // Stack pointer (low byte; stack lives at $0100 + s)
  uint8_t  p;    // Status flags (NV-BDIZC)
} cpu6502_t;

// IRQ priority table (precomputed from the static ROM's IRQ table at $FFC0)
// The JS emulator builds a 64K-entry lookup table; we just keep the 16
// raw vectors and do the priority encoding on the fly.

// System state. Heap-allocated once because it's about 50 KB, which is
// more than we want to put on the stack.
typedef struct tamago_system {
  cpu6502_t cpu;

  // Cycle accumulator for sub-cycle accuracy in step_cycles
  int32_t cycle_budget;

  // Work RAM, display RAM, CPU registers
  uint8_t wram[TAMAGO_WRAM_SIZE];
  uint8_t dram[TAMAGO_DRAM_SIZE];
  uint8_t cpureg[TAMAGO_CPUREG_SIZE];

  // Currently mapped ROM bank (the $4000-$BFFF window). Cached in RAM so
  // the inner CPU loop doesn't have to hit flash on every fetch.
  uint8_t rom_bank_buf[TAMAGO_ROM_BANK_SIZE];
  uint8_t rom_bank_id;  // which bank is currently in rom_bank_buf

  // Static ROM (high 16 KB, mapped at $C000-$FFFF). Loaded once at boot.
  uint8_t static_rom[TAMAGO_STATIC_ROM_SIZE];

  // IRQ vector table: 16 entries, one per IRQ line. Loaded from the
  // static ROM area $FFC0-$FFDF at init. When an IRQ is pending we find
  // the highest-priority bit set in the pending-mask and use that
  // index. The original JS emulator precomputes a 64K-entry lookup
  // table for this; we just do the priority calculation on the fly to
  // save ~128 KB of memory.
  uint16_t irq_vectors[16];

  // Button state (bitmask of TAMAGO_BTN_*). Stored inverted (low = pressed)
  // to match the hardware's pull-up convention.
  uint8_t keys;

  // EEPROM (32 KB external SPI EEPROM on the real device) is NOT kept in
  // RAM — that would push us over Pebble's app heap limit on Emery
  // (~80 KB). Instead the EEPROM is backed directly by Pebble persist
  // storage. The EEPROM emulator (see tamago_eeprom.c) manages a small
  // page cache and writes through to persist on flush.
  uint8_t eeprom_state;

  // Resource handle for the ROM file (kept open across the session)
  ResHandle rom_resource;
  bool rom_loaded;
} tamago_system_t;

// Global system pointer (allocated on the heap by tamago_init).
// Module-private to the emulator; the Pebble app uses the public API.
extern tamago_system_t *g_sys;

// Memory access helpers (defined in tamago_memory.c)
uint8_t  tamago_read(uint16_t addr);
uint16_t tamago_read16(uint16_t addr);
void     tamago_write(uint16_t addr, uint8_t value);

// ROM banking
void tamago_set_rom_bank(uint8_t bank);
bool tamago_load_static_rom(void);

// CPU step (defined in tamago_cpu.c)
uint8_t tamago_cpu_step(void);
void    tamago_cpu_reset(void);
void    tamago_cpu_nmi(void);
void    tamago_cpu_irq(void);

// IRQ logic (defined in tamago_memory.c since it touches cpureg)
void tamago_fire_irq(uint8_t i);
void tamago_fire_nmi(uint8_t i);

#endif // TAMAGO_INTERNAL_H
