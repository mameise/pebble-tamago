/*
 * tamago.h - Tama-Go (Tamagotchi V7) emulator core for Pebble
 *
 * Based on asterick/tamago (JavaScript implementation):
 *   https://github.com/asterick/tamago
 *
 * Hardware: GPLB52320A (8-bit 6502-family) @ 4 MHz
 * Display:  64x32 pixels, 4 grayscale levels (512 bytes DRAM)
 *
 * Memory map (16-bit address space, banked):
 *   $0000-$0FFF  Work RAM (1.5 KB, mirrored)
 *   $1000-$2FFF  Display RAM (512 bytes, mirrored)
 *   $3000-$3FFF  CPU control registers
 *   $4000-$BFFF  Banked ROM (32 KB window, 20 banks total = 640 KB)
 *   $C000-$FFFF  Static ROM (16 KB, always mapped from bank 0)
 *
 * The 640 KB ROM lives in Pebble flash as a resource. To save RAM we
 * only keep the currently-selected bank (32 KB) mirrored in memory.
 * Static ROM (bank 0, first 16 KB) is loaded once at boot.
 */

#ifndef TAMAGO_H
#define TAMAGO_H

#include <pebble.h>

// Display dimensions (native Tama-Go hardware).
//
// The display is 48 pixels wide × 31 pixels high — this comes from the
// HTML template in the JS reference (canvas width=48 height=31). Although
// the hardware has 64 segments × 32 commons, the addressing scheme in
// _dram only gives meaningful pixels in the first 48 of each row; the
// remaining 16 are aliased into the start of the next row's bytes.
// The JS emulator handles this by drawing into a 48-px-wide canvas — the
// 16 "extra" pixels per row spill into the next row and get overwritten.
// In our C port we just clamp to 48 explicitly.
#define TAMAGO_LCD_WIDTH    48
#define TAMAGO_LCD_HEIGHT   31

// 4 grayscale levels per pixel (2 bits each)
#define TAMAGO_PALETTE_SIZE 4

// CPU clock rate (Hz)
#define TAMAGO_CLOCK_RATE   4000000

// Memory region sizes
#define TAMAGO_WRAM_SIZE    0x600   // 1.5 KB work RAM
#define TAMAGO_DRAM_SIZE    0x200   // 512 B display RAM
#define TAMAGO_CPUREG_SIZE  0x100   // 256 B control registers
#define TAMAGO_ROM_BANK_SIZE 0x8000 // 32 KB per bank
#define TAMAGO_STATIC_ROM_SIZE 0x4000  // 16 KB static (high) ROM

// Status flag bits in P register
#define FLAG_C  0x01  // Carry
#define FLAG_Z  0x02  // Zero
#define FLAG_I  0x04  // Interrupt disable
#define FLAG_D  0x08  // Decimal mode
#define FLAG_B  0x10  // Break (software)
#define FLAG_U  0x20  // Unused (always 1)
#define FLAG_V  0x40  // Overflow
#define FLAG_N  0x80  // Negative

// Button bitmask (Tama-Go has 3 buttons: A, B, C plus reset)
#define TAMAGO_BTN_A     0x01
#define TAMAGO_BTN_B     0x02
#define TAMAGO_BTN_C     0x04
#define TAMAGO_BTN_RESET 0x08

// Forward declaration for the system state
typedef struct tamago_system tamago_system_t;

// Public API ---------------------------------------------------------------

// Initialize the emulator. Must be called once at startup. Returns false on
// failure (e.g. ROM resource missing).
bool tamago_init(void);

// Reset the CPU (jumps to the reset vector). Memory and EEPROM are preserved.
void tamago_reset(void);

// Run the emulator for the given number of CPU cycles. Returns the actual
// number of cycles consumed (may differ slightly due to instruction lengths).
uint32_t tamago_step_cycles(uint32_t target_cycles);

// Run a single instruction. Returns the cycle count of that instruction.
uint8_t tamago_step_one(void);

// Read the current display state. Pixels are 2 bits each (0..3 = grayscale
// level). `out` must be at least TAMAGO_LCD_WIDTH * TAMAGO_LCD_HEIGHT bytes.
void tamago_get_display(uint8_t *out);

// Number of status icons surrounding the main display. Layout (matching
// asterick/tamago):
//   Top row:    0 dashboard, 1 food,    2 trash,  3 globe, 4 user
//   Bottom row: 5 comments,  6 medkit,  7 heart,  8 book,  9 bell
#define TAMAGO_ICON_COUNT  10

// Read the 10 status icon values into `out` (length TAMAGO_ICON_COUNT).
// Each byte is a 2-bit intensity (0 = off, 1..3 = visible grayscale).
void tamago_get_icons(uint8_t *out);

// Set the button state. `mask` is a bitmask of TAMAGO_BTN_* constants.
// Bits set = button pressed.
void tamago_set_buttons(uint8_t mask);

// Tear down the emulator and free resources.
void tamago_release(void);

// Debug: return current CPU state for logging. Fills the passed-in pointers
// (any of which may be NULL) with the current register values.
void tamago_debug_get_state(uint16_t *pc, uint8_t *a, uint8_t *x, uint8_t *y,
                            uint8_t *s, uint8_t *bank);

// Debug: return whether any dram bytes are non-zero. If the ROM hasn't
// touched the display at all, all dram = 0 and the screen appears uniform.
bool tamago_debug_dram_dirty(void);

// Debug: count how many DRAM bytes are non-zero (0..512).
uint16_t tamago_debug_dram_nonzero_count(void);

// State serialization for save/restore. Returns total size needed when
// `buf` is NULL or `bufsize` is too small; otherwise the number of bytes
// actually written.
uint32_t tamago_serialize_state(uint8_t *buf, uint32_t bufsize);
bool tamago_deserialize_state(const uint8_t *buf, uint32_t bufsize);

// Direct WRAM read/write. Used by the RTC sync code to poke the
// emulator's internal time counters. `addr` must be in the 0x0000-0x0FFF
// WRAM range — addresses outside that range are silently ignored.
uint8_t tamago_ram_read(uint16_t addr);
void    tamago_ram_write(uint16_t addr, uint8_t val);

// Profiling — snapshot + reset the hot-path counters. Used by main.c
// to print a per-minute profile of where emulator time is going.
// Returns the snapshot via out-params. Pass NULL to skip retrieval.
typedef struct {
  uint32_t opcodes;
  uint32_t reads_fast;
  uint32_t reads_io;
  uint32_t writes_fast;
  uint32_t writes_io;
  uint32_t writes_dropped;
  uint32_t irqs;
  uint32_t nmis;
  uint32_t irq_entries;
  uint32_t nmi_entries;
} tamago_profile_snapshot_t;

void tamago_profile_snapshot_and_reset(tamago_profile_snapshot_t *out);

#endif // TAMAGO_H
