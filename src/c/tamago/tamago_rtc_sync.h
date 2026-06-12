/*
 * tamago_rtc_sync.h
 *
 * Synchronises the Pebble system time (RTC) with the Tama-Go's internal
 * clock counters in WRAM. Without this the Tama drifts away from real
 * time over a long run because the emulator can't quite keep up with
 * the real 4 MHz CPU rate (currently ~75% of full speed on Pebble).
 *
 * Memory map (Tama-Go WRAM, found by ROM disassembly):
 *   $0306: hours    (0-23, plain binary, NOT BCD)
 *   $0307: minutes  (0-59)
 *   $0396: seconds  (0-59)
 *
 * Located by tracing the time-cascade in bank 1 around $56c0:
 *     INC $0396 ; CMP #$3c ; reset ; INC $0307 ; CMP #$3c ; reset ;
 *     INC $0306 ; CMP #$18 ; ...
 *
 * Strategy:
 *   - On app start, write the current Pebble wall-clock time into the
 *     three counters once. The Tama then continues ticking from there.
 *   - Every ~15 minutes, check the drift; if the Tama's clock is more
 *     than 30 seconds off real time, write the real time again.
 *
 * Caveats:
 *   - We don't touch date / year. The Tama-Go has internal date logic
 *     somewhere else; until that's also located, the date inside the
 *     emulator may drift over weeks. Not a problem for short play.
 *   - The IRQ-13 handler runs at 2 Hz and is responsible for ticking
 *     the cascade. If the emulator is briefly paused the Tama clock
 *     will fall behind — periodic re-sync corrects that.
 */

#ifndef TAMAGO_RTC_SYNC_H
#define TAMAGO_RTC_SYNC_H

#include <pebble.h>

// Tama-Go WRAM addresses for the real-time clock cascade.
#define TAMAGO_RTC_HOURS    0x0306
#define TAMAGO_RTC_MINUTES  0x0307
#define TAMAGO_RTC_SECONDS  0x0396

// Drift threshold for periodic resync (seconds). If the Tama's read-back
// time differs from the Pebble wall clock by more than this, we resync.
#define TAMAGO_RTC_DRIFT_THRESHOLD_S  30

// Initial sync: write the current Pebble time into the Tama's clock
// counters. Call this once after tamago_init().
void tamago_rtc_initial_sync(void);

// Periodic check: read back the Tama's clock and compare to the Pebble
// wall clock. Resync if drift exceeds TAMAGO_RTC_DRIFT_THRESHOLD_S. Call
// every ~15 minutes from an AppTimer.
void tamago_rtc_periodic_check(void);

#endif // TAMAGO_RTC_SYNC_H
