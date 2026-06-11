/*
 * tamago_memory.c - memory map, ROM banking, and I/O registers
 *
 * Port of asterick/tamago:
 *   src/tamago/cpu/tamagotchi.js  — memory map, ram/rom helpers
 *   src/tamago/cpu/registers.js   — register write/read handlers
 *
 * Memory map (16-bit address space):
 *   $0000-$0FFF   Work RAM     (1.5 KB, mirrored across 4 KB)
 *   $1000-$2FFF   Display RAM  (512 B, mirrored across 8 KB)
 *   $3000-$3FFF   I/O registers (256 B, mirrored 16x)
 *   $4000-$BFFF   Banked ROM   (32 KB, 20 banks selectable via $3000)
 *   $C000-$FFFF   Static ROM   (16 KB, always mapped from beginning of ROM)
 */

#include "tamago_internal.h"

// We need the resource ID generated from package.json's "media" entry.
// Defined in the generated header `src/resource_ids.auto.h` which is
// included via pebble.h. The Pebble build system generates a constant
// named after the resource (e.g. RESOURCE_ID_TAMA_ROM).
#ifndef RESOURCE_ID_TAMA_ROM
#define RESOURCE_ID_TAMA_ROM 0
#endif

// ----- ROM banking --------------------------------------------------------
//
// 640 KB ROM = 20 banks × 32 KB each. The active bank is mapped at
// $4000-$BFFF. The first 16 KB of the ROM (offset 0x0000-0x3FFF) is also
// mapped permanently at $C000-$FFFF as "static ROM".
//
// We use Pebble's resource_load_byte_range to read pages on demand,
// caching the currently active bank in g_sys->rom_bank_buf.

bool tamago_load_static_rom(void)
{
  // Open the ROM resource and copy the first 16 KB into static_rom.
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

  // Build the IRQ vector table from $FFC0-$FFDF (16 little-endian words).
  // These addresses fall in static ROM: $FFC0 - $C000 = 0x3FC0.
  for (int i = 0; i < 16; i++) {
    uint16_t lo = g_sys->static_rom[0x3FC0 + i * 2];
    uint16_t hi = g_sys->static_rom[0x3FC0 + i * 2 + 1];
    g_sys->irq_vectors[i] = lo | (hi << 8);
  }

  g_sys->rom_loaded = true;
  g_sys->rom_resource = h;
  return true;
}

void tamago_set_rom_bank(uint8_t bank)
{
  // 20 banks total; wrap around if a too-large value is written.
  uint8_t actual = bank % 20;
  if (actual == g_sys->rom_bank_id && g_sys->rom_loaded) {
    return;  // already cached
  }

  if (!g_sys->rom_loaded) {
    APP_LOG(APP_LOG_LEVEL_WARNING,
            "tamago: bank switch %u requested before ROM loaded", bank);
    return;
  }

  // Read 32 KB from the ROM resource into the bank buffer.
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
}

// ----- I/O register handlers ----------------------------------------------
//
// Special registers in $3000-$30FF that need active logic. Anything not
// listed here just reads/writes the underlying cpureg[] array directly.

// $3000: ROM bank control. Writing here changes the active ROM bank.
static void io_write_bank(uint8_t reg, uint8_t value)
{
  g_sys->cpureg[reg] = value;
  tamago_set_rom_bank(value);
}

// $3073, $3074: IRQ pending flags. Writes acknowledge (clear) pending bits.
static void io_write_int_flag(uint8_t reg, uint8_t value)
{
  g_sys->cpureg[reg] &= ~value;
}

// $3011, $3012: Port A direction & data. Configures buttons + IR + cart pins.
static void io_write_porta(uint8_t reg, uint8_t value)
{
  g_sys->cpureg[reg] = value;
  // No active outputs to drive at the moment — the real device routes
  // these to IR, cartridge sense, etc. We'll add as needed.
}

// $3012 read: returns button state OR'd with direction bits.
//
// Hardware behavior (from registers.js):
//   mask = direction register ($3011)
//   value = data register ($3012)
//   spi_power = bit 4 of (mask & value)
//   input = keys | ((spi_power ? 0 : inserted_figure) << 5)
//   return = (mask & value) | (~mask & input)
//
// Bits in `keys`: bit 0=A, bit 1=B, bit 2=C, bit 3=Reset. We store keys
// already in "pull-up" convention: bit=1 means *not pressed* (so the
// idle state matches the OR pattern below). Pressing a button clears
// that bit. tamago_set_buttons() converts the public mask for us.
static uint8_t io_read_porta(uint8_t reg)
{
  (void)reg;
  uint8_t mask  = g_sys->cpureg[0x11];
  uint8_t value = g_sys->cpureg[0x12];
  uint8_t spi_power = mask & value & 0x10;
  // inserted_figure not implemented yet — treat as 0 (no figure).
  uint8_t inserted_figure = 0;
  uint8_t input = g_sys->keys | ((spi_power ? 0 : inserted_figure) << 5);
  return (mask & value) | (~mask & input);
}

// $3015, $3016: Port B direction & data. SPI EEPROM lines (CS/CLK/MOSI).
static void io_write_portb(uint8_t reg, uint8_t value)
{
  g_sys->cpureg[reg] = value;
  // EEPROM SPI update will go here once tamago_eeprom.c is in place.
  // mask = cpureg[0x15], d = ~mask | cpureg[0x16]
  // eeprom_update(d&4, d&2, d&1) — CS, CLK, MOSI bits.
}

static uint8_t io_read_portb(uint8_t reg)
{
  uint8_t mask = g_sys->cpureg[0x15];
  // EEPROM MISO would feed bit 0 here. Stub to 1 (idle high) for now.
  uint8_t eeprom_out = 1;
  return (mask & g_sys->cpureg[reg]) | (~mask & eeprom_out);
}

// ----- Memory read/write --------------------------------------------------
//
// Hot path. We dispatch by the top nibble of the address.

uint8_t tamago_read(uint16_t addr)
{
  uint16_t region = addr >> 12;

  if (region <= 0x0) {
    // $0000-$0FFF: Work RAM (mirrors every 0x600 bytes within 0x1000)
    return g_sys->wram[addr % TAMAGO_WRAM_SIZE];
  }
  if (region <= 0x2) {
    // $1000-$2FFF: Display RAM (mirrors every 0x200 bytes)
    return g_sys->dram[addr % TAMAGO_DRAM_SIZE];
  }
  if (region == 0x3) {
    // $3000-$3FFF: I/O (mirrored every 0x100)
    uint8_t reg = addr & 0xFF;
    switch (reg) {
      case 0x12: return io_read_porta(reg);
      case 0x16: return io_read_portb(reg);
      default:   return g_sys->cpureg[reg];
    }
  }
  if (region <= 0xB) {
    // $4000-$BFFF: Banked ROM
    return g_sys->rom_bank_buf[addr - 0x4000];
  }
  // $C000-$FFFF: Static ROM
  return g_sys->static_rom[addr - 0xC000];
}

uint16_t tamago_read16(uint16_t addr)
{
  uint8_t lo = tamago_read(addr);
  uint8_t hi = tamago_read((addr + 1) & 0xFFFF);
  return lo | (hi << 8);
}

void tamago_write(uint16_t addr, uint8_t value)
{
  uint16_t region = addr >> 12;

  if (region <= 0x0) {
    g_sys->wram[addr % TAMAGO_WRAM_SIZE] = value;
    return;
  }
  if (region <= 0x2) {
    g_sys->dram[addr % TAMAGO_DRAM_SIZE] = value;
    return;
  }
  if (region == 0x3) {
    uint8_t reg = addr & 0xFF;
    switch (reg) {
      case 0x00: io_write_bank(reg, value);     break;
      case 0x11: io_write_porta(reg, value);    break;
      case 0x12: io_write_porta(reg, value);    break;
      case 0x15: io_write_portb(reg, value);    break;
      case 0x16: io_write_portb(reg, value);    break;
      case 0x73: io_write_int_flag(reg, value); break;
      case 0x74: io_write_int_flag(reg, value); break;
      // Silent (no-op) registers from registers.js
      case 0x01: case 0x04: case 0x31:
      case 0x10: case 0x14:
      case 0x70: case 0x71: case 0x76:
        g_sys->cpureg[reg] = value;
        break;
      default:
        // Unknown register — log and store. The JS emulator logs all of
        // these, but logging in the hot path on Pebble would be
        // catastrophic for performance. Silent store for now.
        g_sys->cpureg[reg] = value;
        break;
    }
    return;
  }
  // $4000-$FFFF: ROM, writes ignored.
}

// ----- IRQ / NMI dispatch helpers ----------------------------------------

// Fire IRQ line `i` (0..15). Sets the pending bit in cpureg[$73:$74] if
// the corresponding enable bit in cpureg[$70:$71] is set.
void tamago_fire_irq(uint8_t i)
{
  uint16_t mask = (g_sys->cpureg[0x70] << 8) | g_sys->cpureg[0x71];
  if (!((0x8000 >> i) & mask)) return;
  g_sys->cpureg[0x73 + (i >> 3)] |= 0x80 >> (i & 7);
}

void tamago_fire_nmi(uint8_t i)
{
  // NMI enables live in cpureg[$76], same bit ordering.
  if (~g_sys->cpureg[0x76] & (0x80 >> i)) return;
  tamago_cpu_nmi();
}
