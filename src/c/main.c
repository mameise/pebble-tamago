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

// Display scaling. Hardware is 48×31; on Emery's 200×228 we can comfortably
// go 3x for a sharp tama at 144×93 pixels (or even 4x = 192×124 for a really
// chunky look). Stick with 3x for now to leave headroom for the watchface
// chrome.
#define TAMA_SCALE      3
#define TAMA_DRAW_W     (TAMAGO_LCD_WIDTH  * TAMA_SCALE)
#define TAMA_DRAW_H     (TAMAGO_LCD_HEIGHT * TAMA_SCALE)

// ----- State ----

static Window     *s_window;
static Layer      *s_tama_layer;
static TextLayer  *s_status_layer;
static AppTimer   *s_step_timer;
static char        s_status_text[64];
static uint8_t     s_framebuffer[TAMAGO_LCD_WIDTH * TAMAGO_LCD_HEIGHT];
static uint32_t    s_total_steps;
static bool        s_running;

// 4-level grayscale palette as Pebble GColor values. Indexed 0..3,
// matching the JS PALETTE [0xffdddddd, 0xff9e9e9e, 0xff606060, 0xff222222].
// On color Pebble we approximate with grays; on B&W we'd use the dither.
static GColor s_palette[4];

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

  // Update status line every 5 seconds (rather than every second). The
  // text layer redraw is relatively expensive and isn't time-critical.
  static uint32_t status_ctr = 0;
  if (++status_ctr >= EMU_FPS * 5) {
    status_ctr = 0;
    snprintf(s_status_text, sizeof(s_status_text),
             "Tama-Go: %lu kc", (unsigned long)(s_total_steps / 1000));
    text_layer_set_text(s_status_layer, s_status_text);
  }

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

  // Periodic EEPROM flush every 60 seconds. The EEPROM also flushes
  // automatically on each I²C STOP-after-WRITE, but this catches the
  // case where the watch loses power mid-write or the user force-quits
  // the app. Keeps lost progress to under a minute in the worst case.
  static uint32_t flush_ctr = 0;
  if (++flush_ctr >= EMU_FPS * 60) {
    flush_ctr = 0;
    tamago_eeprom_flush();
  }

  // Redraw the tama layer — but only every 2nd frame (effective 10 fps
  // for the display). The Tama's animations don't need 20 Hz to look
  // smooth, and rendering takes ~10-15ms per frame which is a
  // significant chunk of our budget. Halving it frees CPU time to keep
  // the emulator at full speed.
  static uint8_t render_skip = 0;
  if (s_tama_layer && (++render_skip & 1) == 0) {
    layer_mark_dirty(s_tama_layer);
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

// ----- Window lifecycle ----

static void window_load(Window *window)
{
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  // Status text at the top.
  s_status_layer = text_layer_create(GRect(0, 4, bounds.size.w, 22));
  text_layer_set_text_alignment(s_status_layer, GTextAlignmentCenter);
  text_layer_set_font(s_status_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_background_color(s_status_layer, GColorClear);
  text_layer_set_text_color(s_status_layer, GColorBlack);
  text_layer_set_text(s_status_layer, "Tama-Go starting...");
  layer_add_child(root, text_layer_get_layer(s_status_layer));

  // Tama display layer below.
  s_tama_layer = layer_create(GRect(0, 30, bounds.size.w, bounds.size.h - 30));
  layer_set_update_proc(s_tama_layer, tama_layer_update);
  layer_add_child(root, s_tama_layer);
}

static void window_unload(Window *window)
{
  if (s_tama_layer)   { layer_destroy(s_tama_layer);          s_tama_layer = NULL; }
  if (s_status_layer) { text_layer_destroy(s_status_layer);   s_status_layer = NULL; }
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
    // Show an error window? For now we just exit.
    return;
  }

  s_window = window_create();
#if defined(PBL_COLOR)
  window_set_background_color(s_window, GColorWhite);
#endif
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = window_load,
    .unload = window_unload,
  });
  window_set_click_config_provider(s_window, click_config_provider);
  window_stack_push(s_window, true);

  s_running = true;
  s_step_timer = app_timer_register(EMU_FRAME_MS, step_tick, NULL);
}

static void app_deinit(void)
{
  s_running = false;
  if (s_step_timer) { app_timer_cancel(s_step_timer); s_step_timer = NULL; }
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
