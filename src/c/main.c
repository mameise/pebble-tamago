/*
 * tamagotchi.c - Pebble app shell for the Tama-Go emulator.
 *
 * Phase 1: just verify the emulator initialises, the ROM loads, and we
 * can step the CPU without crashing. Display rendering and button handling
 * are minimal — focus is on getting the build to compile and the bring-up
 * sequence to succeed on real hardware.
 *
 * Display layout for now: full-screen, the Tama LCD (64x32) scaled 2x
 * (128x64) and centered. Status text above shows PC + cycle counter so
 * we can sanity-check that the CPU is actually running.
 */

#include <pebble.h>
#include "tamago/tamago.h"
#include "tamago/tamago_eeprom.h"

// ----- Configuration ----

#define EMU_FPS         20
#define EMU_FRAME_MS    (1000 / EMU_FPS)
#define EMU_CYCLES_PER_FRAME (TAMAGO_CLOCK_RATE / EMU_FPS)

// ----- Layout constants (Emery: 200×228) ----
//
// Watchface-style layout. Top half is the clock face (time + date +
// battery), bottom half is a white "device" panel containing the Tama
// LCD and its status icons. Mirrors the look of the P1 Tamagotchi
// watchface but scaled for Tama-Go's larger native LCD (48×31 vs the
// P1's 32×16).
//
//   y=  6..42   time text       (GOTHIC_28_BOLD, centered)
//   y= 46..66   battery (left) + date (right)
//   y= 76..220  tama panel      (white background)
//     y= 80..94    top icon row
//     y=100..162   tama LCD (96×62, 2× scaled)
//     y=168..182   bottom icon row
//   y=222..228  bottom safe-zone

// Display scaling. 2× gives a 96×62 LCD that fits cleanly into the
// watchface panel without crowding the clock area above.
#define TAMA_SCALE      2
#define TAMA_DRAW_W     (TAMAGO_LCD_WIDTH  * TAMA_SCALE)   // 96
#define TAMA_DRAW_H     (TAMAGO_LCD_HEIGHT * TAMA_SCALE)   // 62

// Status icons: 12×12 each with a 4 px gap.
#define ICON_W           12
#define ICON_H           12
#define ICON_GAP          4
#define ICON_ROW_W       (5 * ICON_W + 4 * ICON_GAP)        // 76

// Panel (white device background behind tama LCD + icons).
#define PANEL_X          20
#define PANEL_Y          76
#define PANEL_W         160
#define PANEL_H         146
#define PANEL_RADIUS      6     // rounded-corner look

// Row positions within the panel (absolute y).
#define ICONS_TOP_Y      80
#define ICONS_TOP_H      14
#define TAMA_LCD_Y      100
#define ICONS_BOT_Y     168
#define ICONS_BOT_H      14

// ----- State ----

static Window      *s_window;
static Layer       *s_tama_bg_layer;     // white panel background
static Layer       *s_tama_layer;        // tama LCD pixels
static Layer       *s_icons_top_layer;
static Layer       *s_icons_bot_layer;
static TextLayer   *s_time_layer;
static TextLayer   *s_date_layer;
static TextLayer   *s_battery_layer;
static AppTimer    *s_step_timer;
static char         s_time_text[16];
static char         s_date_text[16];
static char         s_battery_text[8];
static uint8_t      s_framebuffer[TAMAGO_LCD_WIDTH * TAMAGO_LCD_HEIGHT];
static uint8_t      s_icons[TAMAGO_ICON_COUNT];
static uint32_t     s_total_steps;
static bool        s_running;

// 4-level grayscale palette as Pebble GColor values. Indexed 0..3,
// matching the JS PALETTE [0xffdddddd, 0xff9e9e9e, 0xff606060, 0xff222222].
// On color Pebble we approximate with grays; on B&W we'd use the dither.
static GColor s_palette[4];

// ----- Status icon bitmaps ----
//
// Each icon is a 12×12 monochrome bitmap. Bit 11 of each row word is the
// leftmost pixel (i.e. row & 0x800 = first pixel). Bit 0 is unused —
// keeps the values readable as 12-bit numbers.
//
// Generated from ASCII art (see /tmp/gen_icons.py in the repo history).
// Icons match the FontAwesome glyphs used by the JS reference:
// dashboard, food, trash, globe, user (top row); comments, medkit,
// heart, book, bell (bottom row).

static const uint16_t icon_dashboard[12] = {
  0x000, 0x000, 0x000, 0x7FE, 0x000, 0x7FE,
  0x000, 0x7FE, 0x000, 0x000, 0x000, 0x000,
};
static const uint16_t icon_food[12] = {
  0x000, 0x6D8, 0x6D8, 0x6D8, 0x248, 0x3F0,
  0x0C0, 0x0C0, 0x0C0, 0x0C0, 0x0C0, 0x000,
};
static const uint16_t icon_trash[12] = {
  0x000, 0x1F8, 0x0F0, 0x7FE, 0x7FE, 0x2D0,
  0x2D0, 0x2D0, 0x2D0, 0x2D0, 0x1F8, 0x000,
};
static const uint16_t icon_globe[12] = {
  0x000, 0x0F0, 0x1F8, 0x36C, 0x7FE, 0x3FC,
  0x3FC, 0x7FE, 0x36C, 0x1F8, 0x0F0, 0x000,
};
static const uint16_t icon_user[12] = {
  0x000, 0x060, 0x0F0, 0x0F0, 0x060, 0x000,
  0x1F8, 0x3FC, 0x36C, 0x36C, 0x000, 0x000,
};
static const uint16_t icon_comments[12] = {
  0x000, 0x7F8, 0x618, 0x618, 0x618, 0x7F8,
  0x600, 0x600, 0x000, 0x000, 0x000, 0x000,
};
static const uint16_t icon_medkit[12] = {
  0x000, 0x0F0, 0x0F0, 0x0F0, 0x7FE, 0x7FE,
  0x7FE, 0x0F0, 0x0F0, 0x0F0, 0x000, 0x000,
};
static const uint16_t icon_heart[12] = {
  0x000, 0x30C, 0x79E, 0x7FE, 0x7FE, 0x3FC,
  0x3FC, 0x1F8, 0x0F0, 0x060, 0x000, 0x000,
};
static const uint16_t icon_book[12] = {
  0x000, 0x7FE, 0x6FC, 0x6FC, 0x6FC, 0x6FC,
  0x6FC, 0x6FC, 0x6FC, 0x7FE, 0x000, 0x000,
};
static const uint16_t icon_bell[12] = {
  0x000, 0x060, 0x0F0, 0x1F8, 0x3FC, 0x3FC,
  0x7FE, 0x7FE, 0x7FE, 0x000, 0x0F0, 0x000,
};

// Icon order matches tamago_get_icons output indices. Top row 0..4,
// bottom row 5..9.
static const uint16_t *s_icon_bitmaps[TAMAGO_ICON_COUNT] = {
  icon_dashboard, icon_food, icon_trash,    icon_globe,  icon_user,
  icon_comments,  icon_medkit, icon_heart,  icon_book,   icon_bell,
};

// ----- Display rendering ----
//
// Fast path: capture the Pebble framebuffer once per frame and write our
// scaled pixels directly into it via memset. This is ~50x faster than
// calling graphics_fill_rect for every emulator pixel — at 48×31 scaled
// 3× we'd otherwise be making ~1500 fill_rect calls per frame, which
// completely dominates the frame budget.
//
// Emery uses GBitmapFormat8Bit (1 byte per pixel, full argb8). We can
// blast a horizontal run of identical pixels with a single memset.

static void tama_layer_update(Layer *layer, GContext *ctx)
{
  // Pull the latest emulator framebuffer (48×31 indices into s_palette).
  tamago_get_display(s_framebuffer);

  // The Pebble framebuffer covers the whole window in window coordinates,
  // not layer coordinates. Our tama layer sits at (0, 30) so we add 30
  // to every y we write.
  //
  // graphics_capture_frame_buffer hands us a GBitmap; on Emery this is
  // GBitmapFormat8Bit: row_stride bytes wide, one byte per pixel.
  GBitmap *fb = graphics_capture_frame_buffer(ctx);
  if (!fb) return;

  uint8_t  *data       = gbitmap_get_data(fb);
  uint16_t  row_bytes  = gbitmap_get_bytes_per_row(fb);
  GRect     fb_bounds  = gbitmap_get_bounds(fb);

  // Layer origin within window coordinates.
  Layer *root = window_get_root_layer(s_window);
  GRect layer_frame = layer_get_frame(layer);
  (void)root;  // not strictly needed but documents the relationship
  int layer_x = layer_frame.origin.x;
  int layer_y = layer_frame.origin.y;

  // Tama placement within the layer.
  int ox = layer_x + (layer_frame.size.w - TAMA_DRAW_W) / 2;
  int oy = layer_y + (layer_frame.size.h - TAMA_DRAW_H) / 2;

  // For each emulator pixel, fill a TAMA_SCALE × TAMA_SCALE block. We
  // also pre-cache the palette as raw argb8 bytes so memset works
  // directly without GColor unpacking.
  uint8_t pal_argb[4] = {
    s_palette[0].argb,
    s_palette[1].argb,
    s_palette[2].argb,
    s_palette[3].argb,
  };

  for (int sy = 0; sy < TAMAGO_LCD_HEIGHT; sy++) {
    for (int sx = 0; sx < TAMAGO_LCD_WIDTH; sx++) {
      uint8_t v = pal_argb[s_framebuffer[sy * TAMAGO_LCD_WIDTH + sx] & 0x3];

      // Write a TAMA_SCALE-wide horizontal run on each of the TAMA_SCALE
      // rows. The compiler unrolls these small inner loops with -Os.
      for (int dy = 0; dy < TAMA_SCALE; dy++) {
        int py = oy + sy * TAMA_SCALE + dy;
        if (py < fb_bounds.origin.y || py >= fb_bounds.origin.y + fb_bounds.size.h) continue;
        int px = ox + sx * TAMA_SCALE;
        if (px < fb_bounds.origin.x || px + TAMA_SCALE > fb_bounds.origin.x + fb_bounds.size.w) continue;
        memset(&data[py * row_bytes + px], v, TAMA_SCALE);
      }
    }
  }

  graphics_release_frame_buffer(ctx, fb);
}

// ----- Status icons rendering ----
//
// Renders one row (5 icons) for the given layer. The "first_icon" index
// tells us which slice of s_icons to read — 0 for the top row,
// 5 for the bottom row. We use the same direct-framebuffer write path
// as the tama display since graphics_draw_pixel for ~10*144 pixels per
// frame is slow.
static void icons_row_update(Layer *layer, GContext *ctx, int first_icon)
{
  GBitmap *fb = graphics_capture_frame_buffer(ctx);
  if (!fb) return;

  uint8_t  *data       = gbitmap_get_data(fb);
  uint16_t  row_bytes  = gbitmap_get_bytes_per_row(fb);
  GRect     fb_bounds  = gbitmap_get_bounds(fb);
  GRect     frame      = layer_get_frame(layer);

  // Center the row of icons within the layer.
  int row_x0 = frame.origin.x + (frame.size.w - ICON_ROW_W) / 2;
  int row_y0 = frame.origin.y + (frame.size.h - ICON_H) / 2;

  uint8_t pal_on = s_palette[3].argb;   // darkest gray = "lit" icon

  for (int i = 0; i < 5; i++) {
    uint8_t intensity = s_icons[first_icon + i] & 0x3;
    if (intensity == 0) continue;     // icon is off — skip

    // Pick a palette entry by intensity. The JS uses the same 4-level
    // grayscale; we just map 1→index 1, 2→2, 3→3.
    uint8_t pal_byte = s_palette[intensity].argb;
    (void)pal_on;

    const uint16_t *bm = s_icon_bitmaps[first_icon + i];
    int icon_x0 = row_x0 + i * (ICON_W + ICON_GAP);

    for (int row = 0; row < ICON_H; row++) {
      uint16_t bits = bm[row];
      int py = row_y0 + row;
      if (py < fb_bounds.origin.y ||
          py >= fb_bounds.origin.y + fb_bounds.size.h) continue;

      for (int col = 0; col < ICON_W; col++) {
        if (!(bits & (0x800 >> col))) continue;
        int px = icon_x0 + col;
        if (px < fb_bounds.origin.x ||
            px >= fb_bounds.origin.x + fb_bounds.size.w) continue;
        data[py * row_bytes + px] = pal_byte;
      }
    }
  }

  graphics_release_frame_buffer(ctx, fb);
}

static void icons_top_update(Layer *layer, GContext *ctx)
{
  icons_row_update(layer, ctx, 0);
}

static void icons_bot_update(Layer *layer, GContext *ctx)
{
  icons_row_update(layer, ctx, 5);
}

// ----- Emulator step timer ----

static void step_tick(void *data)
{
  s_step_timer = NULL;
  if (!s_running) return;

  // Time the step so we can see how many real ms we spend emulating.
  time_t epoch_s; uint16_t epoch_ms;
  time_ms(&epoch_s, &epoch_ms);
  uint32_t t0 = (uint32_t)epoch_s * 1000 + epoch_ms;

  // Run one frame's worth of cycles.
  uint32_t spent = tamago_step_cycles(EMU_CYCLES_PER_FRAME);
  s_total_steps += spent;

  time_ms(&epoch_s, &epoch_ms);
  uint32_t t1 = (uint32_t)epoch_s * 1000 + epoch_ms;
  uint32_t step_ms = t1 - t0;

  // Track wall-clock time between successive step_ticks (not just time
  // spent inside step_cycles). This is the true emulation throughput
  // because the AppTimer delay between frames is part of the cost.
  static uint32_t last_t = 0;
  uint32_t wall_ms = (last_t == 0) ? step_ms : (t1 - last_t);
  last_t = t1;

  // Track running totals for a rolling rate measurement.
  static uint32_t bucket_cycles = 0;
  static uint32_t bucket_step_ms = 0;
  static uint32_t bucket_wall_ms = 0;
  bucket_cycles  += spent;
  bucket_step_ms += step_ms;
  bucket_wall_ms += wall_ms;

  // (Old "Tama-Go: XXX kc" debug status text replaced by the real
  // time/date display. Cycle counts still go to APP_LOG below.)

  // Debug log every ~3 seconds: dump CPU state + perf info.
  static uint32_t dbg_ctr = 0;
  if (++dbg_ctr >= EMU_FPS * 3) {
    dbg_ctr = 0;
    uint16_t pc; uint8_t a, x, y, s, bank;
    tamago_debug_get_state(&pc, &a, &x, &y, &s, &bank);
    uint16_t nz = tamago_debug_dram_nonzero_count();
    uint32_t khz_step = bucket_step_ms ? (bucket_cycles / bucket_step_ms) : 0;
    uint32_t khz_wall = bucket_wall_ms ? (bucket_cycles / bucket_wall_ms) : 0;
    APP_LOG(APP_LOG_LEVEL_INFO,
            "perf: %lu cyc | step %lu ms = %lu kHz | wall %lu ms = %lu kHz (target 4000)",
            (unsigned long)bucket_cycles,
            (unsigned long)bucket_step_ms,
            (unsigned long)khz_step,
            (unsigned long)bucket_wall_ms,
            (unsigned long)khz_wall);
    APP_LOG(APP_LOG_LEVEL_INFO,
            "state: PC=$%04x bank=%d nz=%u/512",
            pc, bank, nz);
    bucket_cycles  = 0;
    bucket_step_ms = 0;
    bucket_wall_ms = 0;
  }

  // Periodic EEPROM flush. We mark pages dirty in the bus state machine
  // but defer the slow persist_write_data() to here so write bursts
  // don't tank emulation throughput. Worst-case data loss on a hard
  // crash is one flush interval — 5 minutes is plenty fine for a
  // Tamagotchi (matches what the P1 emulator uses).
  static uint32_t flush_ctr = 0;
  if (++flush_ctr >= EMU_FPS * 300) {  // 5 minutes
    flush_ctr = 0;
    tamago_eeprom_flush();
  }

  // Redraw the tama layer — but only every 2nd frame (effective 10 fps
  // for the display). The Tama's animations don't need 20 Hz to look
  // smooth, and rendering takes ~10-15ms per frame which is a
  // significant chunk of our budget. Halving it frees CPU time to keep
  // the emulator at full speed.
  static uint8_t render_skip = 0;
  if ((++render_skip & 1) == 0) {
    // Pull icon state once per render and stash it for the icon layers.
    tamago_get_icons(s_icons);
    if (s_tama_layer)      layer_mark_dirty(s_tama_layer);
    if (s_icons_top_layer) layer_mark_dirty(s_icons_top_layer);
    if (s_icons_bot_layer) layer_mark_dirty(s_icons_bot_layer);
  }

  // Adaptive frame pacing: aim for one frame every EMU_FRAME_MS ms of
  // wall-clock time. If step_cycles already ate the whole budget (or
  // more), fire the next tick after a small delay (not 0/1 ms!) so the
  // OS scheduler still gets time for housekeeping — especially the
  // backlight controller. Running with delay=1 starves the system and
  // makes the backlight flicker on every button press.
  uint32_t next_delay;
  if (step_ms + 5 >= EMU_FRAME_MS) {
    next_delay = 5;  // already late, but never less than 5 ms
  } else {
    next_delay = EMU_FRAME_MS - step_ms;
  }
  s_step_timer = app_timer_register(next_delay, step_tick, NULL);
}

// ----- Button handlers ----
//
// Tama-Go has 3 buttons: A, B, C. Map to Pebble's Up/Select/Down.
// We use raw click subscribe so the emulator sees press AND release.

static uint8_t s_button_mask = 0;

static void buttons_apply(void) { tamago_set_buttons(s_button_mask); }

// Tell the OS to keep the backlight on while the user is interacting.
// Without this, our tight emulation loop can starve the system's
// backlight subsystem and the light flickers off after each press.
static void poke_backlight(void) { light_enable_interaction(); }

static void btn_a_press(ClickRecognizerRef r, void *ctx)   { s_button_mask |=  TAMAGO_BTN_A; buttons_apply(); poke_backlight(); }
static void btn_a_release(ClickRecognizerRef r, void *ctx) { s_button_mask &= ~TAMAGO_BTN_A; buttons_apply(); }
static void btn_b_press(ClickRecognizerRef r, void *ctx)   { s_button_mask |=  TAMAGO_BTN_B; buttons_apply(); poke_backlight(); }
static void btn_b_release(ClickRecognizerRef r, void *ctx) { s_button_mask &= ~TAMAGO_BTN_B; buttons_apply(); }
static void btn_c_press(ClickRecognizerRef r, void *ctx)   { s_button_mask |=  TAMAGO_BTN_C; buttons_apply(); poke_backlight(); }
static void btn_c_release(ClickRecognizerRef r, void *ctx) { s_button_mask &= ~TAMAGO_BTN_C; buttons_apply(); }

static void click_config_provider(void *context)
{
  window_raw_click_subscribe(BUTTON_ID_UP,     btn_a_press, btn_a_release, NULL);
  window_raw_click_subscribe(BUTTON_ID_SELECT, btn_b_press, btn_b_release, NULL);
  window_raw_click_subscribe(BUTTON_ID_DOWN,   btn_c_press, btn_c_release, NULL);
}

// ----- Time / date / battery updates ----

static void update_time_and_date(struct tm *t)
{
  // Time: HH:MM, following the system 12h/24h preference.
  if (clock_is_24h_style()) {
    strftime(s_time_text, sizeof(s_time_text), "%H:%M", t);
  } else {
    strftime(s_time_text, sizeof(s_time_text), "%I:%M", t);
  }
  // Strip leading zero in 12h mode.
  if (s_time_text[0] == '0') {
    memmove(s_time_text, s_time_text + 1, strlen(s_time_text));
  }
  text_layer_set_text(s_time_layer, s_time_text);

  // Date: e.g. "Fri 12 Jun".
  strftime(s_date_text, sizeof(s_date_text), "%a %d %b", t);
  text_layer_set_text(s_date_layer, s_date_text);
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed)
{
  update_time_and_date(tick_time);
}

static void battery_handler(BatteryChargeState state)
{
  snprintf(s_battery_text, sizeof(s_battery_text), "%d%%", (int)state.charge_percent);
  text_layer_set_text(s_battery_layer, s_battery_text);
}

// ----- Tama-panel background ----
//
// Draws a white rounded-rect "device" background behind the Tama LCD
// and its menu icons. Mirrors the look of the P1 watchface's tama_bg.
static void tama_bg_update_proc(Layer *layer, GContext *ctx)
{
  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, bounds, PANEL_RADIUS, GCornersAll);
  // A thin border so the panel doesn't blend into a white watchface.
  graphics_context_set_stroke_color(ctx, GColorBlack);
  graphics_draw_round_rect(ctx, bounds, PANEL_RADIUS);
}

// ----- Window lifecycle ----

static void window_load(Window *window)
{
  Layer *root = window_get_root_layer(window);
  window_set_background_color(window, GColorLightGray);

  // 1. Time text (large, top).
  s_time_layer = text_layer_create(GRect(0, 4, 200, 40));
  text_layer_set_text_alignment(s_time_layer, GTextAlignmentCenter);
  text_layer_set_font(s_time_layer, fonts_get_system_font(FONT_KEY_BITHAM_30_BLACK));
  text_layer_set_background_color(s_time_layer, GColorClear);
  text_layer_set_text_color(s_time_layer, GColorBlack);
  text_layer_set_text(s_time_layer, "--:--");
  layer_add_child(root, text_layer_get_layer(s_time_layer));

  // 2. Battery (left) and date (right) below the time.
  s_battery_layer = text_layer_create(GRect(6, 46, 70, 22));
  text_layer_set_text_alignment(s_battery_layer, GTextAlignmentLeft);
  text_layer_set_font(s_battery_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_background_color(s_battery_layer, GColorClear);
  text_layer_set_text_color(s_battery_layer, GColorBlack);
  text_layer_set_text(s_battery_layer, "--%");
  layer_add_child(root, text_layer_get_layer(s_battery_layer));

  s_date_layer = text_layer_create(GRect(76, 46, 118, 22));
  text_layer_set_text_alignment(s_date_layer, GTextAlignmentRight);
  text_layer_set_font(s_date_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_background_color(s_date_layer, GColorClear);
  text_layer_set_text_color(s_date_layer, GColorBlack);
  text_layer_set_text(s_date_layer, "");
  layer_add_child(root, text_layer_get_layer(s_date_layer));

  // 3. Tama panel background (white rounded rect).
  s_tama_bg_layer = layer_create(GRect(PANEL_X, PANEL_Y, PANEL_W, PANEL_H));
  layer_set_update_proc(s_tama_bg_layer, tama_bg_update_proc);
  layer_add_child(root, s_tama_bg_layer);

  // 4. Top icons row (inside the panel).
  s_icons_top_layer = layer_create(GRect(0, ICONS_TOP_Y, 200, ICONS_TOP_H));
  layer_set_update_proc(s_icons_top_layer, icons_top_update);
  layer_add_child(root, s_icons_top_layer);

  // 5. Tama LCD (2× scaled = 96×62, centered horizontally).
  s_tama_layer = layer_create(GRect(0, TAMA_LCD_Y, 200, TAMA_DRAW_H));
  layer_set_update_proc(s_tama_layer, tama_layer_update);
  layer_add_child(root, s_tama_layer);

  // 6. Bottom icons row.
  s_icons_bot_layer = layer_create(GRect(0, ICONS_BOT_Y, 200, ICONS_BOT_H));
  layer_set_update_proc(s_icons_bot_layer, icons_bot_update);
  layer_add_child(root, s_icons_bot_layer);

  // Initial time/date/battery populate.
  time_t now = time(NULL);
  update_time_and_date(localtime(&now));
  battery_handler(battery_state_service_peek());
}

static void window_unload(Window *window)
{
  if (s_icons_bot_layer) { layer_destroy(s_icons_bot_layer); s_icons_bot_layer = NULL; }
  if (s_tama_layer)      { layer_destroy(s_tama_layer);      s_tama_layer = NULL; }
  if (s_icons_top_layer) { layer_destroy(s_icons_top_layer); s_icons_top_layer = NULL; }
  if (s_tama_bg_layer)   { layer_destroy(s_tama_bg_layer);   s_tama_bg_layer = NULL; }
  if (s_battery_layer)   { text_layer_destroy(s_battery_layer); s_battery_layer = NULL; }
  if (s_date_layer)      { text_layer_destroy(s_date_layer); s_date_layer = NULL; }
  if (s_time_layer)      { text_layer_destroy(s_time_layer); s_time_layer = NULL; }
}

// ----- App init ----

static void init_palette(void)
{
#if defined(PBL_COLOR)
  // Approximate the JS PALETTE [DD, 9E, 60, 22] with the closest argb8.
  s_palette[0] = GColorFromHEX(0xDDDDDD);  // lightest
  s_palette[1] = GColorFromHEX(0x9E9E9E);
  s_palette[2] = GColorFromHEX(0x606060);
  s_palette[3] = GColorFromHEX(0x222222);  // darkest
#else
  // B&W fallback: 0/1 = white, 2/3 = black. We can do real dithering later.
  s_palette[0] = GColorWhite;
  s_palette[1] = GColorWhite;
  s_palette[2] = GColorBlack;
  s_palette[3] = GColorBlack;
#endif
}

static void app_init(void)
{
  init_palette();

  if (!tamago_init()) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "tamago_init failed -- aborting");
    return;
  }

  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = window_load,
    .unload = window_unload,
  });
  window_set_click_config_provider(s_window, click_config_provider);
  window_stack_push(s_window, true);

  // System time + battery updates. MINUTE_UNIT keeps the watchface live
  // without burning power on per-second redraws — the time text only
  // changes once a minute anyway.
  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
  battery_state_service_subscribe(battery_handler);

  s_running = true;
  s_step_timer = app_timer_register(EMU_FRAME_MS, step_tick, NULL);
}

static void app_deinit(void)
{
  s_running = false;
  if (s_step_timer) { app_timer_cancel(s_step_timer); s_step_timer = NULL; }
  battery_state_service_unsubscribe();
  tick_timer_service_unsubscribe();
  if (s_window)     { window_destroy(s_window);       s_window     = NULL; }
  tamago_release();
}

int main(void)
{
  app_init();
  app_event_loop();
  app_deinit();
  return 0;
}
