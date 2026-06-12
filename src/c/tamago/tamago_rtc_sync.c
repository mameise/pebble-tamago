/*
 * tamago_rtc_sync.c - sync Pebble wall clock into Tama-Go internal RTC.
 */

#include "tamago_rtc_sync.h"
#include "tamago.h"

static void write_pebble_time_to_tama(struct tm *t)
{
  // Tama-Go stores binary 0..23 / 0..59 / 0..59, not BCD.
  tamago_ram_write(TAMAGO_RTC_HOURS,   (uint8_t)t->tm_hour);
  tamago_ram_write(TAMAGO_RTC_MINUTES, (uint8_t)t->tm_min);
  tamago_ram_write(TAMAGO_RTC_SECONDS, (uint8_t)t->tm_sec);

  APP_LOG(APP_LOG_LEVEL_INFO, "rtc_sync: wrote %02d:%02d:%02d to tama",
          t->tm_hour, t->tm_min, t->tm_sec);
}

void tamago_rtc_initial_sync(void)
{
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  if (!t) return;
  write_pebble_time_to_tama(t);

  // Read back immediately to confirm the write took effect at the
  // memory level. If readback fails the address mapping is broken;
  // if it succeeds but the periodic check later sees zeros, the
  // firmware boot code wiped our write — that's the expected case
  // before the user has finished the egg/download flow.
  uint8_t h = tamago_ram_read(TAMAGO_RTC_HOURS);
  uint8_t m = tamago_ram_read(TAMAGO_RTC_MINUTES);
  uint8_t s = tamago_ram_read(TAMAGO_RTC_SECONDS);
  APP_LOG(APP_LOG_LEVEL_INFO,
          "rtc_sync: readback after initial write: %02d:%02d:%02d",
          h, m, s);
}

int32_t tamago_rtc_periodic_check(void)
{
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  if (!t) return 0;

  uint8_t th = tamago_ram_read(TAMAGO_RTC_HOURS);
  uint8_t tm = tamago_ram_read(TAMAGO_RTC_MINUTES);
  uint8_t ts = tamago_ram_read(TAMAGO_RTC_SECONDS);

  // Sanity-check: if the Tama RAM reads back garbage (hours > 23, etc.)
  // we treat it as if the Tama just booted and force a sync. This also
  // handles the case where the ROM hasn't reached the clock-init code yet.
  if (th > 23 || tm > 59 || ts > 59) {
    APP_LOG(APP_LOG_LEVEL_WARNING,
            "rtc_sync: garbage in tama clock (%02x:%02x:%02x), forcing resync",
            th, tm, ts);
    write_pebble_time_to_tama(t);
    return 0;
  }

  // Compute drift for logging only — we always resync.
  int32_t tama_secs = th * 3600 + tm * 60 + ts;
  int32_t real_secs = t->tm_hour * 3600 + t->tm_min * 60 + t->tm_sec;
  int32_t drift = real_secs - tama_secs;
  if (drift >  43200) drift -= 86400;   // wrap around midnight
  if (drift < -43200) drift += 86400;
  int32_t abs_drift = drift < 0 ? -drift : drift;

  // Always log so we can verify the timer is firing on schedule.
  // TODO: drop to "only if drift >= 2" once we've confirmed.
  APP_LOG(APP_LOG_LEVEL_INFO,
          "rtc_sync: tama=%02d:%02d:%02d real=%02d:%02d:%02d drift=%lds (resync)",
          th, tm, ts, t->tm_hour, t->tm_min, t->tm_sec, (long)drift);
  write_pebble_time_to_tama(t);
  return abs_drift;
}
