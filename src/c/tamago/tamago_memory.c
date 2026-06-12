/*
 * tamago_memory.c - memory map, ROM banking, and I/O registers
 *
 * Port of asterick/tamago. Highly optimized for the Pebble's ARM Cortex-M3.
 *
 * Memory map (16-bit address space):
 *   $0000-$0FFF   Work RAM     (1.5 KB, mirrored across 4 KB)
 *   $1000-$2FFF   Display RAM  (512 B, mirrored across 8 KB)
 *   $3000-$3FFF   I/O registers (256 B, mirrored 16x)
 *   $4000-$BFFF   Banked ROM   (32 KB, 20 banks selectable via $3000)
 *   $C000-$FFFF   Static ROM   (16 KB, always mapped from beginning of ROM)
 *
 * Performance: the inner emulation loop hits memory ~3-4 times per
 * instruction. To avoid expensive `addr % size` (modulo, which on M3
 * compiles to a software divide) and function-call overhead, we build a
 * page table at init: one pointer per 256-byte page of the 64K address
 * space, pointing at the backing RAM/ROM buffer. Reads and writes that
 * are pure RAM/ROM become a single array lookup + indirect load. Only
 * the 256 bytes of I/O space ($3000-$30FF) require the slow path with
 * register-specific handlers.
 */

#include "tamago_internal.h"
#include "tamago_eeprom.h"

// We need the resource ID generated from package.json's "media" entry.
// Defined in the generated header `src/resource_ids.auto.h` which is
// included via pebble.h.
#ifndef RESOURCE_ID_TAMA_ROM
#define RESOURCE_ID_TAMA_ROM 0
#endif

// ----- Page table ---------------------------------------------------------
//
// 65536 / 256 = 256 pages. Each page's pointer either points at the start
// of a 256-byte RAM/ROM region OR is NULL to signal "needs slow-path I/O
// handling" (only $3000-$30FF and its 15 mirrors).
//
// Global linkage so the inline accessors in tamago_internal.h reach them.
// Renamed to the public-API names that the header expects.
uint8_t *tamago_read_page[256];
uint8_t *tamago_write_page[256];

// Rebuild the entire page table. Called on init and on every ROM bank switch.
static void rebuild_page_table(void)
{
  // $0000-$0FFF — Work RAM (1.5 KB) mirrored.
  for (int p = 0; p < 16; p++) {
    int wram_page = p % 6;
    tamago_read_page[p]  = g_wram + wram_page * 0x100;
    tamago_write_page[p] = g_wram + wram_page * 0x100;
  }
  // $1000-$2FFF — Display RAM (512 B) mirrored across 32 pages.
  for (int p = 0x10; p < 0x30; p++) {
    tamago_read_page[p]  = g_dram + (p & 1) * 0x100;
    tamago_write_page[p] = g_dram + (p & 1) * 0x100;
  }
  // $3000-$3FFF — I/O registers. NULL = slow path.
  for (int p = 0x30; p < 0x40; p++) {
    tamago_read_page[p]  = NULL;
    tamago_write_page[p] = NULL;
  }
  // $4000-$BFFF — Banked ROM.
  for (int p = 0x40; p < 0xC0; p++) {
    tamago_read_page[p]  = g_rom_bank_buf + (p - 0x40) * 0x100;
    tamago_write_page[p] = NULL;
  }
  // $C000-$FFFF — Static ROM.
  for (int p = 0xC0; p < 0x100; p++) {
    tamago_read_page[p]  = g_static_rom + (p - 0xC0) * 0x100;
    tamago_write_page[p] = NULL;
  }
}

// ----- ROM banking --------------------------------------------------------

bool tamago_load_static_rom(void)
{
  ResHandle h = resource_get_handle(RESOURCE_ID_TAMA_ROM);
  if (!h) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "tamago: ROM resource not found");
    return false;
  }
  size_t total = resource_size(h);
  if (total < TAMAGO_STATIC_ROM_SIZE) {
    APP_LOG(APP_LOG_LEVEL_ERROR,
            "tamago: ROM too small (%u < %u)",
            (unsigned)total, (unsigned)TAMAGO_STATIC_ROM_SIZE);
    return false;
  }

  size_t n = resource_load_byte_range(h, 0, g_static_rom,
                                      TAMAGO_STATIC_ROM_SIZE);
  if (n != TAMAGO_STATIC_ROM_SIZE) {
    APP_LOG(APP_LOG_LEVEL_ERROR,
            "tamago: short read on static ROM (%u/%u)",
            (unsigned)n, (unsigned)TAMAGO_STATIC_ROM_SIZE);
    return false;
  }

  APP_LOG(APP_LOG_LEVEL_INFO,
          "tamago: static ROM loaded (%u bytes), total ROM size %u",
          (unsigned)n, (unsigned)total);

  // Build IRQ vector table from $FFC0-$FFDF.
  for (int i = 0; i < 16; i++) {
    uint16_t lo = g_static_rom[0x3FC0 + i * 2];
    uint16_t hi = g_static_rom[0x3FC0 + i * 2 + 1];
    g_irq_vectors[i] = lo | (hi << 8);
  }

  g_rom_loaded = true;
  g_rom_resource = h;

  // Build the page table now that all backing buffers exist.
  rebuild_page_table();
  return true;
}

void tamago_set_rom_bank(uint8_t bank)
{
  uint8_t actual = bank % 20;
  if (actual == g_rom_bank_id && g_rom_loaded) return;
  if (!g_rom_loaded) return;

  size_t offset = (size_t)actual * TAMAGO_ROM_BANK_SIZE;
  size_t n = resource_load_byte_range(g_rom_resource, offset,
                                      g_rom_bank_buf,
                                      TAMAGO_ROM_BANK_SIZE);
  if (n != TAMAGO_ROM_BANK_SIZE) {
    APP_LOG(APP_LOG_LEVEL_ERROR,
            "tamago: bank %u short read (%u/%u)",
            actual, (unsigned)n, (unsigned)TAMAGO_ROM_BANK_SIZE);
  }
  g_rom_bank_id = actual;
  // The bank buffer pointer hasn't changed — only its contents — so the
  // page table is still valid. No rebuild needed.
}

// ----- I/O register handlers ----------------------------------------------

static void io_write_bank(uint8_t reg, uint8_t value)
{
  g_cpureg[reg] = value;
  tamago_set_rom_bank(value);
}

// $3073, $3074: IRQ pending flags. Writes acknowledge (clear) pending bits.
static void io_write_int_flag(uint8_t reg, uint8_t value)
{
  g_cpureg[reg] &= ~value;
  // Recompute the fast-path cache flag.
  g_irq_pending_any = (g_cpureg[0x73] | g_cpureg[0x74]) != 0;
}

static void io_write_porta(uint8_t reg, uint8_t value)
{
  g_cpureg[reg] = value;
}

static uint8_t io_read_porta(uint8_t reg)
{
  (void)reg;
  uint8_t mask  = g_cpureg[0x11];
  uint8_t value = g_cpureg[0x12];
  uint8_t spi_power = mask & value & 0x10;
  uint8_t inserted_figure = 0;
  uint8_t input = g_keys | ((spi_power ? 0 : inserted_figure) << 5);
  return (mask & value) | (~mask & input);
}

// $3015 (direction) and $3016 (data) writes both end up here. After
// updating the register, we feed the new bus state to the EEPROM bit
// banging engine. Matches write_portb_dir_data in registers.js exactly:
//
//   d = ~mask | data    (input bits read as 1 because of pull-ups)
//   eeprom.update(d & 4, d & 2, d & 1)
//
// Bit 2 = POWER, bit 1 = CLK, bit 0 = DATA.
static void io_write_portb(uint8_t reg, uint8_t value)
{
  g_cpureg[reg] = value;
  uint8_t mask = g_cpureg[0x15];
  uint8_t d    = (uint8_t)(~mask) | g_cpureg[0x16];
  tamago_eeprom_update(d & 4, d & 2, d & 1);
}

// Port B read: bits configured as outputs return what we drove; bits
// configured as inputs read from the EEPROM data line.
//
// (As in the JS reference, the EEPROM output bit is broadcast over all
// input bits via the ~mask term. Only bit 0 is actually meaningful for
// the EEPROM but the firmware only reads bit 0 anyway.)
static uint8_t io_read_portb(uint8_t reg)
{
  uint8_t mask        = g_cpureg[0x15];
  uint8_t eeprom_out  = tamago_eeprom_output();
  uint8_t input_bits  = eeprom_out ? (uint8_t)~mask : 0;
  return (uint8_t)((mask & g_cpureg[reg]) | input_bits);
}

// Slow-path I/O read — called only when tamago_read_page[page] is NULL.
// Renamed from io_read to tamago_io_read so the public inline in
// tamago_internal.h can call it.
uint8_t tamago_io_read(uint16_t addr)
{
  uint8_t reg = addr & 0xFF;
  switch (reg) {
    case 0x12: return io_read_porta(reg);
    case 0x16: return io_read_portb(reg);
    default:   return g_cpureg[reg];
  }
}

// Slow-path I/O write — called only when tamago_write_page[page] is NULL.
void tamago_io_write(uint16_t addr, uint8_t value)
{
  uint8_t reg = addr & 0xFF;
  switch (reg) {
    case 0x00: io_write_bank(reg, value);     break;
    case 0x11: io_write_porta(reg, value);    break;
    case 0x12: io_write_porta(reg, value);    break;
    case 0x15: io_write_portb(reg, value);    break;
    case 0x16: io_write_portb(reg, value);    break;
    case 0x73: io_write_int_flag(reg, value); break;
    case 0x74: io_write_int_flag(reg, value); break;
    default:
      g_cpureg[reg] = value;
      break;
  }
}

// Sound investigation note (kept here for reference):
//   $3060/$3062/$3064/$3065 are written in lockstep 4-tuples — strong
//   candidate for SPU channel control. Re-add an I/O write counter here
//   if/when sound work resumes. Removed to keep hot-path lean and the
//   APP_LOG quiet.

// ----- IRQ / NMI dispatch helpers ----------------------------------------

void tamago_fire_irq(uint8_t i)
{
  g_tamago_profile.irqs++;
  uint16_t mask = (g_cpureg[0x70] << 8) | g_cpureg[0x71];
  if (!((0x8000 >> i) & mask)) return;
  g_cpureg[0x73 + (i >> 3)] |= 0x80 >> (i & 7);
  // Set the fast-path cache flag so the CPU step sees it cheaply.
  g_irq_pending_any = 1;
}

void tamago_fire_nmi(uint8_t i)
{
  g_tamago_profile.nmis++;
  if (~g_cpureg[0x76] & (0x80 >> i)) return;
  tamago_cpu_nmi();
}
