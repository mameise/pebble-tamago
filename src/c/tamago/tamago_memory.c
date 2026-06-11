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
// Static linkage so the inline accessors in tamago_internal.h can see them.
static uint8_t *s_read_page[256];
static uint8_t *s_write_page[256];

// Helper: install the same pointer over a contiguous range of pages.
static void map_pages(uint8_t **table, int first_page, int n_pages, uint8_t *base)
{
  for (int i = 0; i < n_pages; i++) {
    table[first_page + i] = base ? (base + i * 0x100) : NULL;
  }
}

// Rebuild the entire page table. Called on init and on every ROM bank switch.
static void rebuild_page_table(void)
{
  // $0000-$0FFF — Work RAM (1.5 KB) mirrored. The 16 pages of this region
  // map to wram pages [0,1,2,3,4,5, 0,1,2,3,4,5, 0,1,2,3]. We can do this
  // explicitly per page since we have a small loop.
  for (int p = 0; p < 16; p++) {
    int wram_page = p % 6;
    s_read_page[p]  = g_sys->wram + wram_page * 0x100;
    s_write_page[p] = g_sys->wram + wram_page * 0x100;
  }

  // $1000-$2FFF — Display RAM (512 B) mirrored across 32 pages. Pages
  // 0x10..0x2F each map to DRAM page (p & 1) — since DRAM is 2 pages.
  for (int p = 0x10; p < 0x30; p++) {
    s_read_page[p]  = g_sys->dram + (p & 1) * 0x100;
    s_write_page[p] = g_sys->dram + (p & 1) * 0x100;
  }

  // $3000-$3FFF — I/O registers. All 16 pages mirror to cpureg[0..255].
  // BUT we can't use a single pointer because reads/writes need register-
  // specific handlers. Mark NULL → slow path.
  for (int p = 0x30; p < 0x40; p++) {
    s_read_page[p]  = NULL;
    s_write_page[p] = NULL;
  }

  // $4000-$BFFF — Banked ROM (32 KB = 128 pages). Read-only.
  for (int p = 0x40; p < 0xC0; p++) {
    s_read_page[p]  = g_sys->rom_bank_buf + (p - 0x40) * 0x100;
    s_write_page[p] = NULL;  // writes ignored (ROM)
  }

  // $C000-$FFFF — Static ROM (16 KB = 64 pages). Read-only.
  for (int p = 0xC0; p < 0x100; p++) {
    s_read_page[p]  = g_sys->static_rom + (p - 0xC0) * 0x100;
    s_write_page[p] = NULL;  // writes ignored (ROM)
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

  size_t n = resource_load_byte_range(h, 0, g_sys->static_rom,
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
    uint16_t lo = g_sys->static_rom[0x3FC0 + i * 2];
    uint16_t hi = g_sys->static_rom[0x3FC0 + i * 2 + 1];
    g_sys->irq_vectors[i] = lo | (hi << 8);
  }

  g_sys->rom_loaded = true;
  g_sys->rom_resource = h;

  // Build the page table now that all backing buffers exist.
  rebuild_page_table();
  return true;
}

void tamago_set_rom_bank(uint8_t bank)
{
  uint8_t actual = bank % 20;
  if (actual == g_sys->rom_bank_id && g_sys->rom_loaded) return;
  if (!g_sys->rom_loaded) return;

  size_t offset = (size_t)actual * TAMAGO_ROM_BANK_SIZE;
  size_t n = resource_load_byte_range(g_sys->rom_resource, offset,
                                      g_sys->rom_bank_buf,
                                      TAMAGO_ROM_BANK_SIZE);
  if (n != TAMAGO_ROM_BANK_SIZE) {
    APP_LOG(APP_LOG_LEVEL_ERROR,
            "tamago: bank %u short read (%u/%u)",
            actual, (unsigned)n, (unsigned)TAMAGO_ROM_BANK_SIZE);
  }
  g_sys->rom_bank_id = actual;
  // The bank buffer pointer hasn't changed — only its contents — so the
  // page table is still valid. No rebuild needed.
}

// ----- I/O register handlers ----------------------------------------------

static void io_write_bank(uint8_t reg, uint8_t value)
{
  g_sys->cpureg[reg] = value;
  tamago_set_rom_bank(value);
}

static void io_write_int_flag(uint8_t reg, uint8_t value)
{
  g_sys->cpureg[reg] &= ~value;
}

static void io_write_porta(uint8_t reg, uint8_t value)
{
  g_sys->cpureg[reg] = value;
}

static uint8_t io_read_porta(uint8_t reg)
{
  (void)reg;
  uint8_t mask  = g_sys->cpureg[0x11];
  uint8_t value = g_sys->cpureg[0x12];
  uint8_t spi_power = mask & value & 0x10;
  uint8_t inserted_figure = 0;
  uint8_t input = g_sys->keys | ((spi_power ? 0 : inserted_figure) << 5);
  return (mask & value) | (~mask & input);
}

static void io_write_portb(uint8_t reg, uint8_t value)
{
  g_sys->cpureg[reg] = value;
  // EEPROM hookup later.
}

static uint8_t io_read_portb(uint8_t reg)
{
  uint8_t mask = g_sys->cpureg[0x15];
  uint8_t eeprom_out = 1;
  return (mask & g_sys->cpureg[reg]) | (~mask & eeprom_out);
}

// Slow-path I/O read — called only when read_page[page] is NULL.
static uint8_t io_read(uint16_t addr)
{
  uint8_t reg = addr & 0xFF;
  switch (reg) {
    case 0x12: return io_read_porta(reg);
    case 0x16: return io_read_portb(reg);
    default:   return g_sys->cpureg[reg];
  }
}

// Slow-path I/O write — called only when write_page[page] is NULL.
static void io_write(uint16_t addr, uint8_t value)
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
      g_sys->cpureg[reg] = value;
      break;
  }
}

// ----- Public memory accessors --------------------------------------------
//
// Hot path. Each call is one page lookup + one byte load. The compiler
// should inline these aggressively from tamago_cpu.c since they live in
// the same translation unit only if we merge — for now they're function
// calls but each is just ~5 instructions.

uint8_t tamago_read(uint16_t addr)
{
  uint8_t *page = s_read_page[addr >> 8];
  if (page) return page[addr & 0xFF];
  // Slow path: only I/O space ($3000-$30FF mirrored).
  return io_read(addr);
}

uint16_t tamago_read16(uint16_t addr)
{
  uint8_t lo = tamago_read(addr);
  uint8_t hi = tamago_read((addr + 1) & 0xFFFF);
  return lo | (hi << 8);
}

void tamago_write(uint16_t addr, uint8_t value)
{
  uint8_t *page = s_write_page[addr >> 8];
  if (page) { page[addr & 0xFF] = value; return; }
  // Slow path: I/O space, or ROM (write ignored).
  if ((addr >> 12) == 0x3) io_write(addr, value);
  // else: ROM — silently ignore.
}

// ----- IRQ / NMI dispatch helpers ----------------------------------------

void tamago_fire_irq(uint8_t i)
{
  uint16_t mask = (g_sys->cpureg[0x70] << 8) | g_sys->cpureg[0x71];
  if (!((0x8000 >> i) & mask)) return;
  g_sys->cpureg[0x73 + (i >> 3)] |= 0x80 >> (i & 7);
}

void tamago_fire_nmi(uint8_t i)
{
  if (~g_sys->cpureg[0x76] & (0x80 >> i)) return;
  tamago_cpu_nmi();
}
