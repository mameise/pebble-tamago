/*
 * tamago_internal.h - shared state structure for the emulator modules
 *
 * Not exposed to the Pebble app — only included by tamago_*.c files.
 *
 * Performance note: hot-path state (CPU registers, work RAM, display RAM,
 * CPU register array, page tables) is exposed as separate global
 * variables rather than fields of a single g_sys struct. The reason is
 * that GCC treats `g_sys->foo` as a load through a pointer that could be
 * invalidated by any function call (because g_sys itself could in theory
 * be reassigned). With globals it can compute the address at compile time
 * and skip a level of indirection in the inner emulation loop. This
 * matters at ~750,000 memory accesses per emulated frame.
 */

#ifndef TAMAGO_INTERNAL_H
#define TAMAGO_INTERNAL_H

#include "tamago.h"

// 6502 registers (split out as a global, not behind a pointer).
typedef struct {
  uint16_t pc;
  uint8_t  a;
  uint8_t  x;
  uint8_t  y;
  uint8_t  s;
  uint8_t  p;
} cpu6502_t;

extern cpu6502_t g_cpu;

// Memory regions — globals so the compiler can resolve addresses at
// compile time.
extern uint8_t g_wram[TAMAGO_WRAM_SIZE];
extern uint8_t g_dram[TAMAGO_DRAM_SIZE];
extern uint8_t g_cpureg[TAMAGO_CPUREG_SIZE];

// ROM bank cache + static ROM are also globals.
extern uint8_t g_rom_bank_buf[TAMAGO_ROM_BANK_SIZE];
extern uint8_t g_static_rom[TAMAGO_STATIC_ROM_SIZE];

// IRQ vector table (16 entries, loaded once at init).
extern uint16_t g_irq_vectors[16];

// Misc runtime state.
extern uint8_t  g_rom_bank_id;
extern bool     g_rom_loaded;
extern uint8_t  g_keys;
extern ResHandle g_rom_resource;

// Whether the emulator is initialised. Used by tamago_release to know
// whether g_dram etc were ever populated.
extern bool g_initialised;

// ----- Memory access (fast inline path) -----------------------------------
//
// Page table: 256 entries, one per 256-byte page of the 64K address space.
// Each entry points at the backing RAM/ROM buffer for that page, or NULL
// if the page needs slow-path I/O handling.
extern uint8_t *tamago_read_page[256];
extern uint8_t *tamago_write_page[256];

uint8_t tamago_io_read(uint16_t addr);
void    tamago_io_write(uint16_t addr, uint8_t value);

#define TAMAGO_INLINE static inline __attribute__((always_inline))

TAMAGO_INLINE uint8_t tamago_read(uint16_t addr)
{
  uint8_t *page = tamago_read_page[addr >> 8];
  if (page) return page[addr & 0xFF];
  return tamago_io_read(addr);
}

TAMAGO_INLINE uint16_t tamago_read16(uint16_t addr)
{
  uint8_t lo = tamago_read(addr);
  uint8_t hi = tamago_read((addr + 1) & 0xFFFF);
  return lo | (hi << 8);
}

TAMAGO_INLINE void tamago_write(uint16_t addr, uint8_t value)
{
  uint8_t *page = tamago_write_page[addr >> 8];
  if (page) { page[addr & 0xFF] = value; return; }
  if ((addr >> 12) == 0x3) tamago_io_write(addr, value);
}

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
