/*
 * main.c - Pebble watchface shell for the Tama-Go emulator.
 *
 * Replicates the visual layout of the P1 Tamagotchi watchface as closely
 * as possible while adapting to Tama-Go's larger native LCD (48×31 vs
 * the P1's 32×16). Key elements:
 *
 *   1. Hour markers along the edges (12 positions, code-drawn).
 *   2. Digital time + battery + date text.
 *   3. Analog hour + minute hands rotating around screen center,
 *      with outline so they're visible over any background.
 *   4. Tama LCD at 1.5× scale in the lower center, always visible.
 *   5. Dynamic Tama "device" background: only appears when an icon is
 *      lit (menu open) — shrinks back to nothing when the Tama is idle.
 *   6. 10 status icons (5 top + 5 bottom) flanking the LCD; each draws
 *      itself when its 2-bit DRAM intensity is non-zero.
 *
 * Hard-coded defaults for now; the Clay settings page will go in next
 * session.
 */

#include <pebble.h>
#include "tamago/tamago.h"
#include "tamago/tamago_eeprom.h"

// ----- Emulator configuration ----

#define EMU_FPS               20
#define EMU_FRAME_MS          (1000 / EMU_FPS)
#define EMU_CYCLES_PER_FRAME  (TAMAGO_CLOCK_RATE / EMU_FPS)

// ----- Tama LCD layout (2× integer scale) ----
//
// Source: 48×31 (TAMAGO_LCD_WIDTH × TAMAGO_LCD_HEIGHT)
// Dest:   96×62
//
// 2× integer scaling keeps each source pixel a perfectly uniform 2×2
// block in the destination — text and fine LCD segments stay crisp.
// 1.5× / 1.75× looked uneven because their per-column block patterns
// stretched some pixels by 2 and others by 1.

#define TAMA_DRAW_W           96
#define TAMA_DRAW_H           62
#define TAMA_LCD_X            ((200 - TAMA_DRAW_W) / 2)        // 52
#define TAMA_LCD_Y            128                              // moved up
#define TAMA_LCD_X_RIGHT      (TAMA_LCD_X + TAMA_DRAW_W)        // 148
#define TAMA_LCD_Y_BOTTOM     (TAMA_LCD_Y + TAMA_DRAW_H)        // 190

// ----- Icon layout (5 per row, 12×12 each) ----

#define ICON_W                12
#define ICON_H                12
#define ICON_GAP               4
#define ICON_ROW_W            (5 * ICON_W + 4 * ICON_GAP)       // 76
#define ICON_ROW_X            ((200 - ICON_ROW_W) / 2)          // 62

#define ICONS_TOP_Y           112
#define ICONS_BOT_Y           194

// ----- Tama device-frame (dynamic) ----
//
// Width is always anchored to the LCD bounds — at 2× scale the LCD is
// wider than the icon row (96 vs 76), so the icons always fit inside a
// frame sized for the LCD plus a small margin. Only the vertical extent
// of the frame changes based on which icon rows are lit.
#define FRAME_LEFT            (TAMA_LCD_X - 3)
#define FRAME_RIGHT           (TAMA_LCD_X_RIGHT + 3)
#define FRAME_IDLE_TOP        (TAMA_LCD_Y - 3)
#define FRAME_IDLE_BOTTOM     (TAMA_LCD_Y_BOTTOM + 3)
#define FRAME_RADIUS          4

// ----- State ----

static Window      *s_window;
static Layer       *s_bg_layer;          // hour markers + watch-face background
static Layer       *s_tama_bg_layer;     // dynamic white device frame
static Layer       *s_tama_layer;        // tama LCD pixels
static Layer       *s_icons_top_layer;
static Layer       *s_icons_bot_layer;
static Layer       *s_hands_layer;       // analog hands on top
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
static bool         s_running;

// 4-level grayscale palette (matches JS PALETTE).
static GColor s_palette[4];

// ----- Hard-coded style defaults (Clay settings replace these later) ----

// Background fill color and marker color for the watch-face area.
#define BG_FILL_COLOR         GColorLightGray
#define BG_MARKERS_COLOR      GColorBlack

// Marker style: 0 = Arabic ("12"), 1 = Roman ("XII"), 2 = ticks.
#define BG_MARKERS_STYLE      0

// Analog hand colors + thickness.
#define HANDS_COLOR           GColorBlack
#define HANDS_OUTLINE_COLOR   GColorWhite
#define HANDS_INNER_HOUR      4
#define HANDS_INNER_MIN       2

// Tama device frame color (when shown).
#define TAMA_FRAME_FILL       GColorWhite

// ----- Status icon bitmaps (12×12, MSB-first per row) ----

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

static const uint16_t *s_icon_bitmaps[TAMAGO_ICON_COUNT] = {
  icon_dashboard, icon_food, icon_trash,   icon_globe,  icon_user,
  icon_comments,  icon_medkit, icon_heart, icon_book,   icon_bell,
};

// ----- Hour-marker positions (12 markers around the perimeter) ----

typedef struct {
  int16_t x;
  int16_t y;
} marker_pos_t;

static const marker_pos_t MARKER_POSITIONS[12] = {
  /*  0 (12) */ { 100,  10 },
  /*  1 ( 1) */ { 158,  10 },
  /*  2 ( 2) */ { 184,  62 },
  /*  3 ( 3) */ { 184, 114 },
  /*  4 ( 4) */ { 184, 166 },
  /*  5 ( 5) */ { 158, 215 },
  /*  6 ( 6) */ { 100, 215 },
  /*  7 ( 7) */ {  42, 215 },
  /*  8 ( 8) */ {  16, 166 },
  /*  9 ( 9) */ {  16, 114 },
  /* 10 (10) */ {  16,  62 },
  /* 11 (11) */ {  42,  10 },
};

// Both arrays are present even though only one is referenced at build
// time — once the Clay settings page lands, marker style switches at
// runtime and both are needed. Mark unused so -Werror is happy for now.
static const char *const ARABIC_NUMERALS[12] __attribute__((unused)) = {
  "12", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11",
};
static const char *const ROMAN_NUMERALS[12] __attribute__((unused)) = {
  "XII", "I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X", "XI",
};

#define BG_TICK_LENGTH    10

// ----- Watch-face background (BG + hour markers) ----

static void bg_update_proc(Layer *layer, GContext *ctx)
{
  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, BG_FILL_COLOR);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  GFont marker_font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);

  for (int i = 0; i < 12; i++) {
    int16_t mx = MARKER_POSITIONS[i].x;
    int16_t my = MARKER_POSITIONS[i].y;

#if BG_MARKERS_STYLE == 2
    int width = (i % 3 == 0) ? 4 : 2;
    graphics_context_set_stroke_color(ctx, BG_MARKERS_COLOR);
    graphics_context_set_stroke_width(ctx, width);
    GPoint p1, p2;
    if (i == 0 || i == 6) {
      p1 = GPoint(mx, my - BG_TICK_LENGTH / 2);
      p2 = GPoint(mx, my + BG_TICK_LENGTH / 2);
    } else if (i == 3 || i == 9) {
      p1 = GPoint(mx - BG_TICK_LENGTH / 2, my);
      p2 = GPoint(mx + BG_TICK_LENGTH / 2, my);
    } else if (i == 1 || i == 5 || i == 7 || i == 11) {
      p1 = GPoint(mx, my - BG_TICK_LENGTH / 2);
      p2 = GPoint(mx, my + BG_TICK_LENGTH / 2);
    } else {
      p1 = GPoint(mx - BG_TICK_LENGTH / 2, my);
      p2 = GPoint(mx + BG_TICK_LENGTH / 2, my);
    }
    graphics_draw_line(ctx, p1, p2);
#else
    const char *label =
#if BG_MARKERS_STYLE == 1
      ROMAN_NUMERALS[i];
#else
      ARABIC_NUMERALS[i];
#endif
    GRect text_rect = GRect(mx - 16, my - 11, 32, 22);
    graphics_context_set_text_color(ctx, BG_MARKERS_COLOR);
    graphics_draw_text(ctx, label, marker_font, text_rect,
                       GTextOverflowModeWordWrap,
                       GTextAlignmentCenter, NULL);
#endif
  }
}

// ----- Tama device frame (dynamic white panel) ----

// Layer covers the whole window so the rect we draw inside is in absolute
// window coordinates. The rect's width is always FRAME_LEFT..FRAME_RIGHT
// (LCD-anchored); only the vertical extent changes when icons are lit.
static void tama_bg_update_proc(Layer *layer, GContext *ctx)
{
  bool top_lit = false, bot_lit = false;
  for (int i = 0; i < 5; i++) if (s_icons[i])     top_lit = true;
  for (int i = 5; i < 10; i++) if (s_icons[i])    bot_lit = true;

  // No icons → no panel at all. The Tama LCD floats on the watchface.
  if (!top_lit && !bot_lit) return;

  int top    = top_lit ? (ICONS_TOP_Y - 3) : FRAME_IDLE_TOP;
  int bottom = bot_lit ? (ICONS_BOT_Y + ICON_H + 3) : FRAME_IDLE_BOTTOM;

  GRect r = GRect(FRAME_LEFT, top, FRAME_RIGHT - FRAME_LEFT, bottom - top);
  graphics_context_set_fill_color(ctx, TAMA_FRAME_FILL);
  graphics_fill_rect(ctx, r, FRAME_RADIUS, GCornersAll);
}

// ----- Tama LCD rendering (1.5× non-integer scale) ----
//
// Each source pixel maps to a destination block of either 1 or 2 pixels
// in each dimension, based on the integer mapping. We compute the start
// and end of each source pixel's destination block, then memset the
// block to the palette color. Pattern over the 48 columns: 2,1,2,1,...
// = average 1.5x, which the eye reads as a uniform stretch.

static void tama_layer_update(Layer *layer, GContext *ctx)
{
  tamago_get_display(s_framebuffer);

  GBitmap *fb = graphics_capture_frame_buffer(ctx);
  if (!fb) return;
  uint8_t  *data      = gbitmap_get_data(fb);
  uint16_t  row_bytes = gbitmap_get_bytes_per_row(fb);
  GRect     fb_bounds = gbitmap_get_bounds(fb);

  uint8_t pal[4] = { s_palette[0].argb, s_palette[1].argb,
                     s_palette[2].argb, s_palette[3].argb };

  for (int sy = 0; sy < TAMAGO_LCD_HEIGHT; sy++) {
    int dy0 = TAMA_LCD_Y + sy * TAMA_DRAW_H / TAMAGO_LCD_HEIGHT;
    int dy1 = TAMA_LCD_Y + (sy + 1) * TAMA_DRAW_H / TAMAGO_LCD_HEIGHT;

    for (int sx = 0; sx < TAMAGO_LCD_WIDTH; sx++) {
      int dx0 = TAMA_LCD_X + sx * TAMA_DRAW_W / TAMAGO_LCD_WIDTH;
      int dx1 = TAMA_LCD_X + (sx + 1) * TAMA_DRAW_W / TAMAGO_LCD_WIDTH;
      uint8_t v = pal[s_framebuffer[sy * TAMAGO_LCD_WIDTH + sx] & 0x3];

      for (int dy = dy0; dy < dy1; dy++) {
        if (dy < fb_bounds.origin.y) continue;
        if (dy >= fb_bounds.origin.y + fb_bounds.size.h) continue;
        if (dx0 < fb_bounds.origin.x) continue;
        if (dx1 > fb_bounds.origin.x + fb_bounds.size.w) continue;
        memset(&data[dy * row_bytes + dx0], v, dx1 - dx0);
      }
    }
  }

  graphics_release_frame_buffer(ctx, fb);
}

// ----- Status icon row rendering ----

static void icons_row_update(Layer *layer, GContext *ctx, int first_icon)
{
  GBitmap *fb = graphics_capture_frame_buffer(ctx);
  if (!fb) return;
  uint8_t  *data      = gbitmap_get_data(fb);
  uint16_t  row_bytes = gbitmap_get_bytes_per_row(fb);
  GRect     fb_bounds = gbitmap_get_bounds(fb);
  GRect     frame     = layer_get_frame(layer);

  int row_y = frame.origin.y;

  for (int i = 0; i < 5; i++) {
    uint8_t intensity = s_icons[first_icon + i] & 0x3;
    if (intensity == 0) continue;

    uint8_t pal_byte = s_palette[intensity].argb;
    const uint16_t *bm = s_icon_bitmaps[first_icon + i];
    int icon_x0 = ICON_ROW_X + i * (ICON_W + ICON_GAP);

    for (int row = 0; row < ICON_H; row++) {
      int py = row_y + row;
      if (py < fb_bounds.origin.y ||
          py >= fb_bounds.origin.y + fb_bounds.size.h) continue;
      uint16_t bits = bm[row];
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

// ----- Analog hands ----

static void hands_update_proc(Layer *layer, GContext *ctx)
{
  GRect bounds = layer_get_bounds(layer);
  GPoint center = GPoint(bounds.size.w / 2, bounds.size.h / 2);

  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  if (!t) return;

  int hour   = t->tm_hour % 12;
  int minute = t->tm_min;

  int32_t hour_angle = TRIG_MAX_ANGLE * (hour * 60 + minute) / (12 * 60);
  int32_t min_angle  = TRIG_MAX_ANGLE * minute / 60;

  int hh_len = 38;
  int mh_len = 60;

  GPoint hour_end = {
    .x = (int16_t)(sin_lookup(hour_angle) * hh_len / TRIG_MAX_RATIO) + center.x,
    .y = (int16_t)(-cos_lookup(hour_angle) * hh_len / TRIG_MAX_RATIO) + center.y,
  };
  GPoint min_end = {
    .x = (int16_t)(sin_lookup(min_angle) * mh_len / TRIG_MAX_RATIO) + center.x,
    .y = (int16_t)(-cos_lookup(min_angle) * mh_len / TRIG_MAX_RATIO) + center.y,
  };

  // Hour hand: outline first, then inner color.
  graphics_context_set_stroke_color(ctx, HANDS_OUTLINE_COLOR);
  graphics_context_set_stroke_width(ctx, HANDS_INNER_HOUR + 2);
  graphics_draw_line(ctx, center, hour_end);
  graphics_context_set_stroke_color(ctx, HANDS_COLOR);
  graphics_context_set_stroke_width(ctx, HANDS_INNER_HOUR);
  graphics_draw_line(ctx, center, hour_end);

  // Minute hand.
  graphics_context_set_stroke_color(ctx, HANDS_OUTLINE_COLOR);
  graphics_context_set_stroke_width(ctx, HANDS_INNER_MIN + 2);
  graphics_draw_line(ctx, center, min_end);
  graphics_context_set_stroke_color(ctx, HANDS_COLOR);
  graphics_context_set_stroke_width(ctx, HANDS_INNER_MIN);
  graphics_draw_line(ctx, center, min_end);

  // Center dot.
  graphics_context_set_fill_color(ctx, HANDS_OUTLINE_COLOR);
  graphics_fill_circle(ctx, center, 5);
  graphics_context_set_fill_color(ctx, HANDS_COLOR);
  graphics_fill_circle(ctx, center, 3);
}

// ----- Emulator step timer ----

static void step_tick(void *data)
{
  s_step_timer = NULL;
  if (!s_running) return;

  time_t epoch_s; uint16_t epoch_ms;
  time_ms(&epoch_s, &epoch_ms);
  uint32_t t0 = (uint32_t)epoch_s * 1000 + epoch_ms;

  uint32_t spent = tamago_step_cycles(EMU_CYCLES_PER_FRAME);
  s_total_steps += spent;

  time_ms(&epoch_s, &epoch_ms);
  uint32_t t1 = (uint32_t)epoch_s * 1000 + epoch_ms;
  uint32_t step_ms = t1 - t0;

  // Periodic EEPROM flush every 5 minutes.
  static uint32_t flush_ctr = 0;
  if (++flush_ctr >= EMU_FPS * 300) {
    flush_ctr = 0;
    tamago_eeprom_flush();
  }

  // Debug log every ~3 seconds.
  static uint32_t dbg_ctr = 0;
  if (++dbg_ctr >= EMU_FPS * 3) {
    dbg_ctr = 0;
    uint16_t pc; uint8_t a, x, y, s, bank;
    tamago_debug_get_state(&pc, &a, &x, &y, &s, &bank);
    APP_LOG(APP_LOG_LEVEL_INFO,
            "tamago: PC=$%04x bank=%d steps=%lukc step=%lums",
            pc, bank, (unsigned long)(s_total_steps / 1000),
            (unsigned long)step_ms);
  }

  // Render every 2nd frame (10 fps display).
  static uint8_t render_skip = 0;
  if ((++render_skip & 1) == 0) {
    tamago_get_icons(s_icons);
    if (s_tama_bg_layer)    layer_mark_dirty(s_tama_bg_layer);
    if (s_tama_layer)       layer_mark_dirty(s_tama_layer);
    if (s_icons_top_layer)  layer_mark_dirty(s_icons_top_layer);
    if (s_icons_bot_layer)  layer_mark_dirty(s_icons_bot_layer);
  }

  // Adaptive frame pacing — give the OS at least 5 ms between frames.
  uint32_t next_delay;
  if (step_ms + 5 >= EMU_FRAME_MS) {
    next_delay = 5;
  } else {
    next_delay = EMU_FRAME_MS - step_ms;
  }
  s_step_timer = app_timer_register(next_delay, step_tick, NULL);
}

// ----- Time / date / battery updates ----

static void update_time_and_date(struct tm *t)
{
  if (clock_is_24h_style()) {
    strftime(s_time_text, sizeof(s_time_text), "%H:%M", t);
  } else {
    strftime(s_time_text, sizeof(s_time_text), "%I:%M", t);
    if (s_time_text[0] == '0') {
      memmove(s_time_text, s_time_text + 1, strlen(s_time_text));
    }
  }
  text_layer_set_text(s_time_layer, s_time_text);

  strftime(s_date_text, sizeof(s_date_text), "%a %d %b", t);
  text_layer_set_text(s_date_layer, s_date_text);

  // Hands need to redraw too.
  if (s_hands_layer) layer_mark_dirty(s_hands_layer);
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed)
{
  update_time_and_date(tick_time);
}

static void battery_handler(BatteryChargeState state)
{
  snprintf(s_battery_text, sizeof(s_battery_text), "%d%%",
           (int)state.charge_percent);
  text_layer_set_text(s_battery_layer, s_battery_text);
}

// ----- Buttons ----

static uint8_t s_button_mask = 0;
static void buttons_apply(void) { tamago_set_buttons(s_button_mask); }
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

  // Z-order (back to front):
  //   1. Watchface BG + hour markers (full window)
  //   2. Time text          (y=30..62)
  //   3. Battery text       (left, y=70..88)
  //   4. Date text          (right, y=70..88)
  //   5. Tama device frame  (dynamic, only when icons lit)
  //   6. Tama LCD pixels    (rect at TAMA_LCD_X..Y)
  //   7. Top status icons   (y=ICONS_TOP_Y)
  //   8. Bottom status icons (y=ICONS_BOT_Y)
  //   9. Analog hands       (full window, drawn on top of everything)

  s_bg_layer = layer_create(GRect(0, 0, 200, 228));
  layer_set_update_proc(s_bg_layer, bg_update_proc);
  layer_add_child(root, s_bg_layer);

  s_time_layer = text_layer_create(GRect(0, 30, 200, 36));
  text_layer_set_text_alignment(s_time_layer, GTextAlignmentCenter);
  text_layer_set_font(s_time_layer, fonts_get_system_font(FONT_KEY_BITHAM_30_BLACK));
  text_layer_set_background_color(s_time_layer, GColorClear);
  text_layer_set_text_color(s_time_layer, GColorBlack);
  text_layer_set_text(s_time_layer, "--:--");
  layer_add_child(root, text_layer_get_layer(s_time_layer));

  s_battery_layer = text_layer_create(GRect(35, 70, 70, 20));
  text_layer_set_text_alignment(s_battery_layer, GTextAlignmentLeft);
  text_layer_set_font(s_battery_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_background_color(s_battery_layer, GColorClear);
  text_layer_set_text_color(s_battery_layer, GColorBlack);
  text_layer_set_text(s_battery_layer, "--%");
  layer_add_child(root, text_layer_get_layer(s_battery_layer));

  // Date needs ~85 px to fit "Fri 12 Jun" at GOTHIC_18_BOLD; 56 px clipped
  // to "Fri 12 ...". Match P1 layout exactly.
  s_date_layer = text_layer_create(GRect(95, 70, 85, 20));
  text_layer_set_text_alignment(s_date_layer, GTextAlignmentRight);
  text_layer_set_font(s_date_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_background_color(s_date_layer, GColorClear);
  text_layer_set_text_color(s_date_layer, GColorBlack);
  text_layer_set_text(s_date_layer, "");
  layer_add_child(root, text_layer_get_layer(s_date_layer));

  // Tama device frame (dynamic — full-window so we draw at absolute coords).
  s_tama_bg_layer = layer_create(GRect(0, 0, 200, 228));
  layer_set_update_proc(s_tama_bg_layer, tama_bg_update_proc);
  layer_add_child(root, s_tama_bg_layer);

  // Tama LCD area (just big enough to contain the rendered pixels).
  s_tama_layer = layer_create(GRect(TAMA_LCD_X, TAMA_LCD_Y, TAMA_DRAW_W, TAMA_DRAW_H));
  layer_set_update_proc(s_tama_layer, tama_layer_update);
  // We render to the full framebuffer though (clip-checked); the layer
  // bounds are used only for the layer-system's mark-dirty bookkeeping.
  layer_set_clips(s_tama_layer, false);
  layer_add_child(root, s_tama_layer);

  s_icons_top_layer = layer_create(GRect(0, ICONS_TOP_Y, 200, ICON_H));
  layer_set_update_proc(s_icons_top_layer, icons_top_update);
  layer_set_clips(s_icons_top_layer, false);
  layer_add_child(root, s_icons_top_layer);

  s_icons_bot_layer = layer_create(GRect(0, ICONS_BOT_Y, 200, ICON_H));
  layer_set_update_proc(s_icons_bot_layer, icons_bot_update);
  layer_set_clips(s_icons_bot_layer, false);
  layer_add_child(root, s_icons_bot_layer);

  // Hands on top — full window so they can sweep anywhere.
  s_hands_layer = layer_create(GRect(0, 0, 200, 228));
  layer_set_update_proc(s_hands_layer, hands_update_proc);
  layer_add_child(root, s_hands_layer);

  // Populate text now so nothing reads "--:--" before the first tick.
  time_t now = time(NULL);
  update_time_and_date(localtime(&now));
  battery_handler(battery_state_service_peek());
}

static void window_unload(Window *window)
{
  if (s_hands_layer)     { layer_destroy(s_hands_layer);      s_hands_layer = NULL; }
  if (s_icons_bot_layer) { layer_destroy(s_icons_bot_layer);  s_icons_bot_layer = NULL; }
  if (s_icons_top_layer) { layer_destroy(s_icons_top_layer);  s_icons_top_layer = NULL; }
  if (s_tama_layer)      { layer_destroy(s_tama_layer);       s_tama_layer = NULL; }
  if (s_tama_bg_layer)   { layer_destroy(s_tama_bg_layer);    s_tama_bg_layer = NULL; }
  if (s_date_layer)      { text_layer_destroy(s_date_layer);  s_date_layer = NULL; }
  if (s_battery_layer)   { text_layer_destroy(s_battery_layer); s_battery_layer = NULL; }
  if (s_time_layer)      { text_layer_destroy(s_time_layer);  s_time_layer = NULL; }
  if (s_bg_layer)        { layer_destroy(s_bg_layer);         s_bg_layer = NULL; }
}

// ----- App init / deinit ----

static void init_palette(void)
{
#if defined(PBL_COLOR)
  s_palette[0] = GColorFromHEX(0xDDDDDD);
  s_palette[1] = GColorFromHEX(0x9E9E9E);
  s_palette[2] = GColorFromHEX(0x606060);
  s_palette[3] = GColorFromHEX(0x222222);
#else
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
  if (s_window) { window_destroy(s_window); s_window = NULL; }
  tamago_release();
}

int main(void)
{
  app_init();
  app_event_loop();
  app_deinit();
  return 0;
}
