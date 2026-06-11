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

// ----- Configuration ----

#define EMU_FPS         20
#define EMU_FRAME_MS    (1000 / EMU_FPS)
#define EMU_CYCLES_PER_FRAME (TAMAGO_CLOCK_RATE / EMU_FPS)

// Display scaling (Emery only for now).
#define TAMA_SCALE      2
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

static void tama_layer_update(Layer *layer, GContext *ctx)
{
  // Fetch the latest framebuffer from the emulator.
  tamago_get_display(s_framebuffer);

  GRect bounds = layer_get_bounds(layer);
  // Center the scaled tama within the layer.
  int ox = (bounds.size.w - TAMA_DRAW_W) / 2;
  int oy = (bounds.size.h - TAMA_DRAW_H) / 2;

  for (int y = 0; y < TAMAGO_LCD_HEIGHT; y++) {
    for (int x = 0; x < TAMAGO_LCD_WIDTH; x++) {
      uint8_t pix = s_framebuffer[y * TAMAGO_LCD_WIDTH + x] & 0x3;
      graphics_context_set_fill_color(ctx, s_palette[pix]);
      graphics_fill_rect(ctx,
                         GRect(ox + x * TAMA_SCALE, oy + y * TAMA_SCALE,
                               TAMA_SCALE, TAMA_SCALE),
                         0, GCornerNone);
    }
  }
}

// ----- Emulator step timer ----

static void step_tick(void *data)
{
  s_step_timer = NULL;
  if (!s_running) return;

  // Run one frame's worth of cycles.
  uint32_t spent = tamago_step_cycles(EMU_CYCLES_PER_FRAME);
  s_total_steps += spent;

  // Update status line (don't do this every frame — keep it cheap).
  static uint32_t status_ctr = 0;
  if (++status_ctr >= EMU_FPS) {  // ~once per second
    status_ctr = 0;
    snprintf(s_status_text, sizeof(s_status_text),
             "Tama-Go: %lu kc", (unsigned long)(s_total_steps / 1000));
    text_layer_set_text(s_status_layer, s_status_text);
  }

  // Redraw and schedule next frame.
  if (s_tama_layer) layer_mark_dirty(s_tama_layer);
  s_step_timer = app_timer_register(EMU_FRAME_MS, step_tick, NULL);
}

// ----- Button handlers ----
//
// Tama-Go has 3 buttons: A, B, C. Map to Pebble's Up/Select/Down.
// We use raw click subscribe so the emulator sees press AND release.

static uint8_t s_button_mask = 0;

static void buttons_apply(void) { tamago_set_buttons(s_button_mask); }

static void btn_a_press(ClickRecognizerRef r, void *ctx)   { s_button_mask |=  TAMAGO_BTN_A; buttons_apply(); }
static void btn_a_release(ClickRecognizerRef r, void *ctx) { s_button_mask &= ~TAMAGO_BTN_A; buttons_apply(); }
static void btn_b_press(ClickRecognizerRef r, void *ctx)   { s_button_mask |=  TAMAGO_BTN_B; buttons_apply(); }
static void btn_b_release(ClickRecognizerRef r, void *ctx) { s_button_mask &= ~TAMAGO_BTN_B; buttons_apply(); }
static void btn_c_press(ClickRecognizerRef r, void *ctx)   { s_button_mask |=  TAMAGO_BTN_C; buttons_apply(); }
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
