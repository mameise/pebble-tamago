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
#include "tamago/tamago_rtc_sync.h"

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
// Shadow text layers used to fake an outline effect (4 offsets per text).
// Attached BELOW their primary so the primary draws on top.
static TextLayer   *s_time_shadow[4]    = { NULL, NULL, NULL, NULL };
static TextLayer   *s_date_shadow[4]    = { NULL, NULL, NULL, NULL };
static TextLayer   *s_battery_shadow[4] = { NULL, NULL, NULL, NULL };
static AppTimer    *s_step_timer;
static char         s_time_text[16];
static char         s_date_text[16];
static char         s_battery_text[8];
static uint8_t      s_framebuffer[TAMAGO_LCD_WIDTH * TAMAGO_LCD_HEIGHT];
static uint8_t      s_icons[TAMAGO_ICON_COUNT];

// Dirty-tracking shadows: previous frame's LCD pixels and icon states.
// Compared after each emulator step; we only mark visual layers dirty
// when their inputs actually changed. Saves a full re-composite of the
// (heavy) layer stack on identical frames.
static uint8_t      s_framebuffer_prev[TAMAGO_LCD_WIDTH * TAMAGO_LCD_HEIGHT];
static uint8_t      s_icons_prev[TAMAGO_ICON_COUNT];
static bool         s_last_top_lit;
static bool         s_last_bot_lit;
static bool         s_dirty_state_init = false;   // force first-paint
static uint32_t     s_total_steps;
static bool         s_running;

// Runtime settings — defined fully + loaded in load_settings() later.
// Forward-declared here so the update_proc functions earlier in the file
// can reference them.
static bool    s_tama_frame_enabled = true;
static GColor  s_tama_frame_color;
static GColor  s_tama_pixel_color;
static GColor  s_bg_fill_color;
static GColor  s_bg_markers_color;
static uint8_t s_bg_markers_style;     // 0=Arabic, 1=Roman, 2=Ticks
static GColor  s_hands_color;
static GColor  s_hands_outline_color;
static uint8_t s_hands_thickness;      // 0=Thick, 1=Medium, 2=Thin
static GColor  s_text_color;
static bool    s_text_outline_enabled;
static GColor  s_text_outline_color;

// 4-level grayscale palette (matches JS PALETTE).
static GColor s_palette[4];

// ----- Hard-coded style defaults (Clay settings replace these progressively) ----

// Background fill / hour-marker color + style are now Clay settings:
// s_bg_fill_color, s_bg_markers_color, s_bg_markers_style.

// Analog hand colors + thickness are now Clay settings:
// s_hands_color, s_hands_outline_color, s_hands_thickness.

// Time/date/battery text color + outline are now Clay settings:
// s_text_color, s_text_outline_enabled, s_text_outline_color.

// Tama device frame color is now a setting (s_tama_frame_color),
// initialised in load_settings() and updated via AppMessage.

// ----- Status icon bitmaps (12×12, MSB-first per row) ----
//
// These mirror the actual Tama-Go LCD menu icons (Information, Food,
// Toilet, Doors, Training, IR, Medicine, Gotchi Figure, Album, Attention)
// based on the Bandai Quick Start Guide labels and TamaTalk forum
// descriptions. Pixel art is approximate — the real LCD uses fixed
// segments, we render onto a 12×12 grid.

static const uint16_t icon_status[12] = {
  // small character face — Information / status menu
  0x000, 0x1F8, 0x204, 0x50A, 0x402, 0x402,
  0x4F2, 0x402, 0x204, 0x1F8, 0x000, 0x000,
};
static const uint16_t icon_food[12] = {
  // chef hat — feeding menu
  0x000, 0x1F8, 0x3FC, 0x7FE, 0x7FE, 0x7FE,
  0x3FC, 0x3FC, 0x3FC, 0x3FC, 0x000, 0x000,
};
static const uint16_t icon_toilet[12] = {
  // toilet bowl with seat — cleanup
  0x000, 0x000, 0x7FE, 0x402, 0x3FC, 0x204,
  0x204, 0x204, 0x1F8, 0x108, 0x1F8, 0x000,
};
static const uint16_t icon_door[12] = {
  // door with handle — Doors menu (go outside)
  0x000, 0x7FE, 0x606, 0x606, 0x606, 0x606,
  0x61E, 0x606, 0x606, 0x606, 0x7FE, 0x000,
};
static const uint16_t icon_training[12] = {
  // X mark — Training / Time-Out
  0x000, 0x402, 0x606, 0x30C, 0x198, 0x0F0,
  0x0F0, 0x198, 0x30C, 0x606, 0x402, 0x000,
};
static const uint16_t icon_ir[12] = {
  // two arrows pointing inward — IR communication
  0x000, 0x000, 0x108, 0x30C, 0x70E, 0xF0F,
  0x70E, 0x30C, 0x108, 0x000, 0x000, 0x000,
};
static const uint16_t icon_medicine[12] = {
  // diagonal syringe — Medicine
  0x000, 0x006, 0x00E, 0x01C, 0x038, 0x1F0,
  0x3E0, 0x780, 0xE00, 0xC00, 0x400, 0x000,
};
static const uint16_t icon_heart[12] = {
  // heart — Gotchi Figure / friend
  0x000, 0x000, 0x30C, 0x79E, 0x7FE, 0x7FE,
  0x7FE, 0x3FC, 0x1F8, 0x0F0, 0x060, 0x000,
};
static const uint16_t icon_book[12] = {
  // book with pages — Album / Friends list
  0x000, 0x7FE, 0x4F2, 0x402, 0x4F2, 0x402,
  0x4F2, 0x402, 0x4F2, 0x402, 0x7FE, 0x000,
};
static const uint16_t icon_bell[12] = {
  // bell with clapper — Attention indicator
  0x000, 0x060, 0x0F0, 0x1F8, 0x1F8, 0x3FC,
  0x3FC, 0x7FE, 0x7FE, 0xFFF, 0x060, 0x0F0,
};

// Pointer table in DRAM-bit order:
// Top row    (DRAM[4] bit 0-1 + DRAM[5] bits): status, food, toilet, door, training
// Bottom row (DRAM[6] bits + DRAM[7] bit 6-7):  ir, medicine, heart, book, bell
static const uint16_t *s_icon_bitmaps[TAMAGO_ICON_COUNT] = {
  icon_status, icon_food,     icon_toilet, icon_door, icon_training,
  icon_ir,     icon_medicine, icon_heart,  icon_book, icon_bell,
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

// Both arrays are now used at runtime based on s_bg_markers_style.
static const char *const ARABIC_NUMERALS[12] = {
  "12", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11",
};
static const char *const ROMAN_NUMERALS[12] = {
  "XII", "I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X", "XI",
};

#define BG_TICK_LENGTH    10

// ----- Watch-face background (BG + hour markers) ----

static void bg_update_proc(Layer *layer, GContext *ctx)
{
  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, s_bg_fill_color);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  GFont marker_font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);

  for (int i = 0; i < 12; i++) {
    int16_t mx = MARKER_POSITIONS[i].x;
    int16_t my = MARKER_POSITIONS[i].y;

    if (s_bg_markers_style == 2) {
      // Ticks: short line at each marker position, longer for cardinals.
      int width = (i % 3 == 0) ? 4 : 2;
      graphics_context_set_stroke_color(ctx, s_bg_markers_color);
      graphics_context_set_stroke_width(ctx, width);
      // Determine tick orientation based on which edge the marker sits on.
      // Top/bottom markers: vertical tick. Left/right markers: horizontal.
      bool vertical = (i == 0 || i == 6 || i == 1 || i == 5 || i == 7 || i == 11);
      GPoint p1, p2;
      if (vertical) {
        p1 = GPoint(mx, my - BG_TICK_LENGTH / 2);
        p2 = GPoint(mx, my + BG_TICK_LENGTH / 2);
      } else {
        p1 = GPoint(mx - BG_TICK_LENGTH / 2, my);
        p2 = GPoint(mx + BG_TICK_LENGTH / 2, my);
      }
      graphics_draw_line(ctx, p1, p2);
    } else {
      // Arabic (0) or Roman (1) numerals drawn as text.
      const char *label = (s_bg_markers_style == 1)
                        ? ROMAN_NUMERALS[i]
                        : ARABIC_NUMERALS[i];
      GRect text_rect = GRect(mx - 16, my - 11, 32, 22);
      graphics_context_set_text_color(ctx, s_bg_markers_color);
      graphics_draw_text(ctx, label, marker_font, text_rect,
                         GTextOverflowModeWordWrap,
                         GTextAlignmentCenter, NULL);
    }
  }
}

// ----- Tama device frame (dynamic white panel) ----

// Layer covers the whole window so the rect we draw inside is in absolute
// window coordinates. The rect's width is always FRAME_LEFT..FRAME_RIGHT
// (LCD-anchored); only the vertical extent changes when icons are lit.
// If the user disabled the frame in settings, we never draw.
static void tama_bg_update_proc(Layer *layer, GContext *ctx)
{
  if (!s_tama_frame_enabled) return;

  bool top_lit = false, bot_lit = false;
  for (int i = 0; i < 5; i++) if (s_icons[i])     top_lit = true;
  for (int i = 5; i < 10; i++) if (s_icons[i])    bot_lit = true;

  // No icons → no panel at all. The Tama LCD floats on the watchface.
  if (!top_lit && !bot_lit) return;

  int top    = top_lit ? (ICONS_TOP_Y - 3) : FRAME_IDLE_TOP;
  int bottom = bot_lit ? (ICONS_BOT_Y + ICON_H + 3) : FRAME_IDLE_BOTTOM;

  GRect r = GRect(FRAME_LEFT, top, FRAME_RIGHT - FRAME_LEFT, bottom - top);
  graphics_context_set_fill_color(ctx, s_tama_frame_color);
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
  // s_framebuffer is populated by step_tick before mark_dirty. No need
  // to re-pull it here — that would just memcpy DRAM redundantly.

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

  // Hand thickness (inner stroke) per setting. The outline draws 2 px
  // wider so the colored hand always has a visible border.
  //   0 = Thick   inner 5 / outer 7
  //   1 = Medium  inner 4 / outer 6   (default, matches old hardcoded)
  //   2 = Thin    inner 3 / outer 5
  static const uint8_t HOUR_INNER[3] = { 5, 4, 3 };
  static const uint8_t MIN_INNER [3] = { 3, 2, 2 };
  uint8_t style = s_hands_thickness < 3 ? s_hands_thickness : 1;
  uint8_t hour_inner = HOUR_INNER[style];
  uint8_t hour_outer = hour_inner + 2;
  uint8_t min_inner  = MIN_INNER[style];
  uint8_t min_outer  = min_inner + 2;

  // Hour hand: outline first, then inner color.
  graphics_context_set_stroke_color(ctx, s_hands_outline_color);
  graphics_context_set_stroke_width(ctx, hour_outer);
  graphics_draw_line(ctx, center, hour_end);
  graphics_context_set_stroke_color(ctx, s_hands_color);
  graphics_context_set_stroke_width(ctx, hour_inner);
  graphics_draw_line(ctx, center, hour_end);

  // Minute hand.
  graphics_context_set_stroke_color(ctx, s_hands_outline_color);
  graphics_context_set_stroke_width(ctx, min_outer);
  graphics_draw_line(ctx, center, min_end);
  graphics_context_set_stroke_color(ctx, s_hands_color);
  graphics_context_set_stroke_width(ctx, min_inner);
  graphics_draw_line(ctx, center, min_end);

  // Center dot.
  graphics_context_set_fill_color(ctx, s_hands_outline_color);
  graphics_fill_circle(ctx, center, 5);
  graphics_context_set_fill_color(ctx, s_hands_color);
  graphics_fill_circle(ctx, center, 3);
}

// ----- Attention detection (vibration on bell-icon rising edge) ----
//
// The bell icon (icon 9, DRAM[7] bits 7:6) lights up when the Tama wants
// attention — hungry, sick, needs cleaning, etc. We mirror the P1 watch-
// face's UX: vibrate ONCE per attention "event", and stay quiet while the
// user navigates the menus to dismiss it.
//
// Strategy: detect the rising edge of icon[9] (off→on). Fire vibration,
// then disarm. Once the icon turns off again, re-arm for the next event.
//
// A short cooldown protects against rapid icon flicker, in case the
// firmware blinks the bell while drawing.
//
// Later, when sound is added, we'll move to the full P1 logic: only fire
// when the buzzer goes on AND attention icon is set. For now, icon-only
// is good enough.
#define VIBE_COOLDOWN_S    5
static bool   s_prev_attention = false;
static time_t s_last_vibe_time = 0;
static bool   s_vibration_enabled = true;   // Clay setting (loaded in load_settings)

static void check_attention_and_vibrate(void)
{
  bool attention = s_icons[9] != 0;

  if (attention && !s_prev_attention) {
    // Rising edge — Tama just started asking for attention.
    time_t now = time(NULL);
    if (s_vibration_enabled && (now - s_last_vibe_time) >= VIBE_COOLDOWN_S) {
      vibes_long_pulse();
      s_last_vibe_time = now;
      APP_LOG(APP_LOG_LEVEL_INFO, "attention: bell icon on, vibrating");
    }
  }
  s_prev_attention = attention;
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

  // Debug log every ~60 seconds.
  static uint32_t dbg_ctr = 0;
  if (++dbg_ctr >= EMU_FPS * 60) {
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
    check_attention_and_vibrate();

    // ---- Dirty tracking ----
    // Pull the LCD pixels and compare to last frame. Only mark layers
    // dirty when their inputs actually changed; on Pebble the whole
    // window composites every time, so saving a mark_dirty saves the
    // entire stack (bg + tama_bg + tama_lcd + icons + text+shadows +
    // hands) from being redrawn.
    tamago_get_display(s_framebuffer);

    // Tama LCD layer — pixel diff
    bool lcd_changed = !s_dirty_state_init ||
        memcmp(s_framebuffer, s_framebuffer_prev, sizeof(s_framebuffer)) != 0;

    // Icons layers — separate top/bot since they're independent
    bool icons_top_changed = !s_dirty_state_init ||
        memcmp(&s_icons[0], &s_icons_prev[0], 5) != 0;
    bool icons_bot_changed = !s_dirty_state_init ||
        memcmp(&s_icons[5], &s_icons_prev[5], 5) != 0;

    // Tama-frame visibility derived from icon row activity
    bool top_lit = (s_icons[0] | s_icons[1] | s_icons[2] |
                    s_icons[3] | s_icons[4]) != 0;
    bool bot_lit = (s_icons[5] | s_icons[6] | s_icons[7] |
                    s_icons[8] | s_icons[9]) != 0;
    bool frame_changed = !s_dirty_state_init ||
        (top_lit != s_last_top_lit) || (bot_lit != s_last_bot_lit);

    if (lcd_changed) {
      memcpy(s_framebuffer_prev, s_framebuffer, sizeof(s_framebuffer));
      if (s_tama_layer) layer_mark_dirty(s_tama_layer);
    }
    if (icons_top_changed) {
      memcpy(&s_icons_prev[0], &s_icons[0], 5);
      if (s_icons_top_layer) layer_mark_dirty(s_icons_top_layer);
    }
    if (icons_bot_changed) {
      memcpy(&s_icons_prev[5], &s_icons[5], 5);
      if (s_icons_bot_layer) layer_mark_dirty(s_icons_bot_layer);
    }
    if (frame_changed) {
      s_last_top_lit = top_lit;
      s_last_bot_lit = bot_lit;
      if (s_tama_bg_layer) layer_mark_dirty(s_tama_bg_layer);
    }
    s_dirty_state_init = true;
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

// Helper: push a string to a primary text layer and all its 4 shadows.
static void set_text_with_shadows(TextLayer *primary, TextLayer **shadows,
                                  const char *text)
{
  if (primary) text_layer_set_text(primary, text);
  for (int i = 0; i < 4; i++) {
    if (shadows[i]) text_layer_set_text(shadows[i], text);
  }
}

// Helper: apply the current text-color / outline-color / outline-enabled
// settings to all text layers + shadows. Idempotent — safe to call
// multiple times.
static void apply_text_style(void)
{
  if (s_time_layer)    text_layer_set_text_color(s_time_layer,    s_text_color);
  if (s_date_layer)    text_layer_set_text_color(s_date_layer,    s_text_color);
  if (s_battery_layer) text_layer_set_text_color(s_battery_layer, s_text_color);

  TextLayer **all_shadows[3] = { s_time_shadow, s_date_shadow, s_battery_shadow };
  for (int t = 0; t < 3; t++) {
    for (int i = 0; i < 4; i++) {
      TextLayer *sh = all_shadows[t][i];
      if (!sh) continue;
      text_layer_set_text_color(sh, s_text_outline_color);
      layer_set_hidden(text_layer_get_layer(sh), !s_text_outline_enabled);
    }
  }
}

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
  set_text_with_shadows(s_time_layer, s_time_shadow, s_time_text);

  strftime(s_date_text, sizeof(s_date_text), "%a %d %b", t);
  set_text_with_shadows(s_date_layer, s_date_shadow, s_date_text);

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
  set_text_with_shadows(s_battery_layer, s_battery_shadow, s_battery_text);
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
  //   2. Time shadow ×4   (only visible when TextOutline enabled)
  //   3. Time text        (primary, y=30..62)
  //   4. Battery shadow ×4
  //   5. Battery text     (primary, left, y=70..88)
  //   6. Date shadow ×4
  //   7. Date text        (primary, right, y=70..88)
  //   8. Tama device frame  (dynamic, only when icons lit)
  //   9. Tama LCD pixels    (rect at TAMA_LCD_X..Y)
  //   10. Top status icons  (y=ICONS_TOP_Y)
  //   11. Bottom status icons (y=ICONS_BOT_Y)
  //   12. Analog hands      (full window, drawn on top of everything)

  s_bg_layer = layer_create(GRect(0, 0, 200, 228));
  layer_set_update_proc(s_bg_layer, bg_update_proc);
  layer_add_child(root, s_bg_layer);

  // Offsets for the 4 shadow positions around each primary text layer.
  // NW, NE, SW, SE — gives a 1px outline on all sides when rendered.
  static const int SHADOW_OFFSETS[4][2] = {
    {-1, -1}, { 1, -1}, {-1,  1}, { 1,  1}
  };

  GFont time_font = fonts_get_system_font(FONT_KEY_BITHAM_30_BLACK);
  GFont small_font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);

  // ---- Time + shadows ----
  GRect time_frame = GRect(0, 30, 200, 36);
  for (int i = 0; i < 4; i++) {
    GRect r = time_frame;
    r.origin.x += SHADOW_OFFSETS[i][0];
    r.origin.y += SHADOW_OFFSETS[i][1];
    s_time_shadow[i] = text_layer_create(r);
    text_layer_set_text_alignment(s_time_shadow[i], GTextAlignmentCenter);
    text_layer_set_font(s_time_shadow[i], time_font);
    text_layer_set_background_color(s_time_shadow[i], GColorClear);
    text_layer_set_text_color(s_time_shadow[i], GColorWhite);
    text_layer_set_text(s_time_shadow[i], "--:--");
    layer_set_hidden(text_layer_get_layer(s_time_shadow[i]), true);
    layer_add_child(root, text_layer_get_layer(s_time_shadow[i]));
  }
  s_time_layer = text_layer_create(time_frame);
  text_layer_set_text_alignment(s_time_layer, GTextAlignmentCenter);
  text_layer_set_font(s_time_layer, time_font);
  text_layer_set_background_color(s_time_layer, GColorClear);
  text_layer_set_text_color(s_time_layer, GColorBlack);
  text_layer_set_text(s_time_layer, "--:--");
  layer_add_child(root, text_layer_get_layer(s_time_layer));

  // ---- Battery + shadows ----
  GRect battery_frame = GRect(35, 70, 70, 20);
  for (int i = 0; i < 4; i++) {
    GRect r = battery_frame;
    r.origin.x += SHADOW_OFFSETS[i][0];
    r.origin.y += SHADOW_OFFSETS[i][1];
    s_battery_shadow[i] = text_layer_create(r);
    text_layer_set_text_alignment(s_battery_shadow[i], GTextAlignmentLeft);
    text_layer_set_font(s_battery_shadow[i], small_font);
    text_layer_set_background_color(s_battery_shadow[i], GColorClear);
    text_layer_set_text_color(s_battery_shadow[i], GColorWhite);
    text_layer_set_text(s_battery_shadow[i], "--%");
    layer_set_hidden(text_layer_get_layer(s_battery_shadow[i]), true);
    layer_add_child(root, text_layer_get_layer(s_battery_shadow[i]));
  }
  s_battery_layer = text_layer_create(battery_frame);
  text_layer_set_text_alignment(s_battery_layer, GTextAlignmentLeft);
  text_layer_set_font(s_battery_layer, small_font);
  text_layer_set_background_color(s_battery_layer, GColorClear);
  text_layer_set_text_color(s_battery_layer, GColorBlack);
  text_layer_set_text(s_battery_layer, "--%");
  layer_add_child(root, text_layer_get_layer(s_battery_layer));

  // ---- Date + shadows ----
  // Date needs ~85 px to fit "Fri 12 Jun" at GOTHIC_18_BOLD.
  GRect date_frame = GRect(95, 70, 85, 20);
  for (int i = 0; i < 4; i++) {
    GRect r = date_frame;
    r.origin.x += SHADOW_OFFSETS[i][0];
    r.origin.y += SHADOW_OFFSETS[i][1];
    s_date_shadow[i] = text_layer_create(r);
    text_layer_set_text_alignment(s_date_shadow[i], GTextAlignmentRight);
    text_layer_set_font(s_date_shadow[i], small_font);
    text_layer_set_background_color(s_date_shadow[i], GColorClear);
    text_layer_set_text_color(s_date_shadow[i], GColorWhite);
    text_layer_set_text(s_date_shadow[i], "");
    layer_set_hidden(text_layer_get_layer(s_date_shadow[i]), true);
    layer_add_child(root, text_layer_get_layer(s_date_shadow[i]));
  }
  s_date_layer = text_layer_create(date_frame);
  text_layer_set_text_alignment(s_date_layer, GTextAlignmentRight);
  text_layer_set_font(s_date_layer, small_font);
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

  // Apply persisted text-color + outline settings to all layers.
  apply_text_style();
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
  for (int i = 0; i < 4; i++) {
    if (s_time_shadow[i])    { text_layer_destroy(s_time_shadow[i]);    s_time_shadow[i]    = NULL; }
    if (s_date_shadow[i])    { text_layer_destroy(s_date_shadow[i]);    s_date_shadow[i]    = NULL; }
    if (s_battery_shadow[i]) { text_layer_destroy(s_battery_shadow[i]); s_battery_shadow[i] = NULL; }
  }
  if (s_bg_layer)        { layer_destroy(s_bg_layer);         s_bg_layer = NULL; }
}

// ----- Persistent settings ------------------------------------------------
//
// Keys 1-99 reserved for settings. EEPROM uses 100..115.

#define PERSIST_KEY_VIBRATION_ENABLED    1
#define PERSIST_KEY_TAMA_FRAME_ENABLED   2
#define PERSIST_KEY_TAMA_FRAME_COLOR     3
#define PERSIST_KEY_TAMA_PIXEL_COLOR     4
#define PERSIST_KEY_BG_FILL_COLOR        5
#define PERSIST_KEY_BG_MARKERS_COLOR     6
#define PERSIST_KEY_BG_MARKERS_STYLE     7
#define PERSIST_KEY_HANDS_COLOR          8
#define PERSIST_KEY_HANDS_OUTLINE_COLOR  9
#define PERSIST_KEY_HANDS_THICKNESS     10
#define PERSIST_KEY_TEXT_COLOR          11
#define PERSIST_KEY_TEXT_OUTLINE        12
#define PERSIST_KEY_TEXT_OUTLINE_COLOR  13

// Runtime setting state — declarations are at the top of the file
// (forward-declared so the early update_proc functions can use them).
// s_vibration_enabled lives in the attention block above.

// Re-derive the 4-shade greyscale palette from a single user-chosen
// "darkest pixel" color. We blend toward white in 4 equal steps:
//   shade 0 (lightest, LCD-off background) = pure white
//   shade 3 (darkest)                       = user's choice
// Aplite (no color) keeps the binary scheme.
static void update_palette_from_color(GColor user_color)
{
#if defined(PBL_COLOR)
  uint8_t r_user = user_color.r;   // 0..3 (2 bits per channel on Pebble)
  uint8_t g_user = user_color.g;
  uint8_t b_user = user_color.b;
  for (int i = 0; i < 4; i++) {
    int r = (3 * (3 - i) + r_user * i + 1) / 3;
    int g = (3 * (3 - i) + g_user * i + 1) / 3;
    int b = (3 * (3 - i) + b_user * i + 1) / 3;
    if (r > 3) r = 3;
    if (g > 3) g = 3;
    if (b > 3) b = 3;
    if (r < 0) r = 0;
    if (g < 0) g = 0;
    if (b < 0) b = 0;
    s_palette[i].a = 3;
    s_palette[i].r = (uint8_t)r;
    s_palette[i].g = (uint8_t)g;
    s_palette[i].b = (uint8_t)b;
  }
#else
  (void)user_color;
  s_palette[0] = GColorWhite;
  s_palette[1] = GColorWhite;
  s_palette[2] = GColorBlack;
  s_palette[3] = GColorBlack;
#endif
}

static void load_settings(void)
{
  s_vibration_enabled  = persist_exists(PERSIST_KEY_VIBRATION_ENABLED)
                       ? persist_read_bool(PERSIST_KEY_VIBRATION_ENABLED) : true;
  s_tama_frame_enabled = persist_exists(PERSIST_KEY_TAMA_FRAME_ENABLED)
                       ? persist_read_bool(PERSIST_KEY_TAMA_FRAME_ENABLED) : true;

  if (persist_exists(PERSIST_KEY_TAMA_FRAME_COLOR)) {
    int v = persist_read_int(PERSIST_KEY_TAMA_FRAME_COLOR);
    s_tama_frame_color = (GColor){ .argb = (uint8_t)v };
  } else {
    s_tama_frame_color = GColorWhite;
  }

  if (persist_exists(PERSIST_KEY_TAMA_PIXEL_COLOR)) {
    int v = persist_read_int(PERSIST_KEY_TAMA_PIXEL_COLOR);
    s_tama_pixel_color = (GColor){ .argb = (uint8_t)v };
  } else {
    s_tama_pixel_color = GColorBlack;
  }

  if (persist_exists(PERSIST_KEY_BG_FILL_COLOR)) {
    int v = persist_read_int(PERSIST_KEY_BG_FILL_COLOR);
    s_bg_fill_color = (GColor){ .argb = (uint8_t)v };
  } else {
    s_bg_fill_color = GColorLightGray;
  }

  if (persist_exists(PERSIST_KEY_BG_MARKERS_COLOR)) {
    int v = persist_read_int(PERSIST_KEY_BG_MARKERS_COLOR);
    s_bg_markers_color = (GColor){ .argb = (uint8_t)v };
  } else {
    s_bg_markers_color = GColorBlack;
  }

  s_bg_markers_style = persist_exists(PERSIST_KEY_BG_MARKERS_STYLE)
                     ? (uint8_t)persist_read_int(PERSIST_KEY_BG_MARKERS_STYLE)
                     : 0;
  if (s_bg_markers_style > 2) s_bg_markers_style = 0;

  if (persist_exists(PERSIST_KEY_HANDS_COLOR)) {
    int v = persist_read_int(PERSIST_KEY_HANDS_COLOR);
    s_hands_color = (GColor){ .argb = (uint8_t)v };
  } else {
    s_hands_color = GColorBlack;
  }

  if (persist_exists(PERSIST_KEY_HANDS_OUTLINE_COLOR)) {
    int v = persist_read_int(PERSIST_KEY_HANDS_OUTLINE_COLOR);
    s_hands_outline_color = (GColor){ .argb = (uint8_t)v };
  } else {
    s_hands_outline_color = GColorWhite;
  }

  s_hands_thickness = persist_exists(PERSIST_KEY_HANDS_THICKNESS)
                    ? (uint8_t)persist_read_int(PERSIST_KEY_HANDS_THICKNESS)
                    : 1;
  if (s_hands_thickness > 2) s_hands_thickness = 1;

  if (persist_exists(PERSIST_KEY_TEXT_COLOR)) {
    int v = persist_read_int(PERSIST_KEY_TEXT_COLOR);
    s_text_color = (GColor){ .argb = (uint8_t)v };
  } else {
    s_text_color = GColorBlack;
  }

  s_text_outline_enabled = persist_exists(PERSIST_KEY_TEXT_OUTLINE)
                         ? persist_read_bool(PERSIST_KEY_TEXT_OUTLINE)
                         : false;

  if (persist_exists(PERSIST_KEY_TEXT_OUTLINE_COLOR)) {
    int v = persist_read_int(PERSIST_KEY_TEXT_OUTLINE_COLOR);
    s_text_outline_color = (GColor){ .argb = (uint8_t)v };
  } else {
    s_text_outline_color = GColorWhite;
  }

  update_palette_from_color(s_tama_pixel_color);
}

static void save_settings(void)
{
  persist_write_bool(PERSIST_KEY_VIBRATION_ENABLED,    s_vibration_enabled);
  persist_write_bool(PERSIST_KEY_TAMA_FRAME_ENABLED,   s_tama_frame_enabled);
  persist_write_int (PERSIST_KEY_TAMA_FRAME_COLOR,     s_tama_frame_color.argb);
  persist_write_int (PERSIST_KEY_TAMA_PIXEL_COLOR,     s_tama_pixel_color.argb);
  persist_write_int (PERSIST_KEY_BG_FILL_COLOR,        s_bg_fill_color.argb);
  persist_write_int (PERSIST_KEY_BG_MARKERS_COLOR,     s_bg_markers_color.argb);
  persist_write_int (PERSIST_KEY_BG_MARKERS_STYLE,     s_bg_markers_style);
  persist_write_int (PERSIST_KEY_HANDS_COLOR,          s_hands_color.argb);
  persist_write_int (PERSIST_KEY_HANDS_OUTLINE_COLOR,  s_hands_outline_color.argb);
  persist_write_int (PERSIST_KEY_HANDS_THICKNESS,      s_hands_thickness);
  persist_write_int (PERSIST_KEY_TEXT_COLOR,           s_text_color.argb);
  persist_write_bool(PERSIST_KEY_TEXT_OUTLINE,         s_text_outline_enabled);
  persist_write_int (PERSIST_KEY_TEXT_OUTLINE_COLOR,   s_text_outline_color.argb);
}

// ----- AppMessage (Clay settings round-trip) ------------------------------
//
// Clay packs each setting into a Tuple under its messageKey. Numeric keys
// (toggles) come through as Boolean / Int; color pickers send a 32-bit
// 0xRRGGBB integer that we convert to a Pebble GColor.

static void inbox_received_handler(DictionaryIterator *iter, void *context)
{
  Tuple *t;
  bool changed_palette = false;

  t = dict_find(iter, MESSAGE_KEY_VibrationEnabled);
  if (t) {
    s_vibration_enabled = (t->value->int8 != 0);
    APP_LOG(APP_LOG_LEVEL_INFO, "settings: VibrationEnabled = %d",
            (int)s_vibration_enabled);
  }

  t = dict_find(iter, MESSAGE_KEY_TamaFrameEnabled);
  if (t) {
    s_tama_frame_enabled = (t->value->int8 != 0);
    APP_LOG(APP_LOG_LEVEL_INFO, "settings: TamaFrameEnabled = %d",
            (int)s_tama_frame_enabled);
  }

  t = dict_find(iter, MESSAGE_KEY_TamaFrameColor);
  if (t) {
    uint32_t hex = (uint32_t)t->value->int32;
    s_tama_frame_color = GColorFromHEX(hex);
    APP_LOG(APP_LOG_LEVEL_INFO, "settings: TamaFrameColor = 0x%06lx",
            (unsigned long)hex);
  }

  t = dict_find(iter, MESSAGE_KEY_TamaPixelColor);
  if (t) {
    uint32_t hex = (uint32_t)t->value->int32;
    s_tama_pixel_color = GColorFromHEX(hex);
    changed_palette = true;
    APP_LOG(APP_LOG_LEVEL_INFO, "settings: TamaPixelColor = 0x%06lx",
            (unsigned long)hex);
  }

  t = dict_find(iter, MESSAGE_KEY_BgFillColor);
  if (t) {
    uint32_t hex = (uint32_t)t->value->int32;
    s_bg_fill_color = GColorFromHEX(hex);
    APP_LOG(APP_LOG_LEVEL_INFO, "settings: BgFillColor = 0x%06lx",
            (unsigned long)hex);
  }

  t = dict_find(iter, MESSAGE_KEY_BgMarkersColor);
  if (t) {
    uint32_t hex = (uint32_t)t->value->int32;
    s_bg_markers_color = GColorFromHEX(hex);
    APP_LOG(APP_LOG_LEVEL_INFO, "settings: BgMarkersColor = 0x%06lx",
            (unsigned long)hex);
  }

  t = dict_find(iter, MESSAGE_KEY_BgMarkersStyle);
  if (t) {
    // Clay "select" sends the option's value as a C-string (e.g. "0", "1", "2").
    uint8_t style = 0;
    if (t->type == TUPLE_CSTRING && t->length > 0) {
      style = (uint8_t)atoi(t->value->cstring);
    } else if (t->type == TUPLE_INT) {
      style = (uint8_t)t->value->int32;
    }
    if (style > 2) style = 0;
    s_bg_markers_style = style;
    APP_LOG(APP_LOG_LEVEL_INFO, "settings: BgMarkersStyle = %d",
            (int)s_bg_markers_style);
  }

  t = dict_find(iter, MESSAGE_KEY_HandsColor);
  if (t) {
    uint32_t hex = (uint32_t)t->value->int32;
    s_hands_color = GColorFromHEX(hex);
    APP_LOG(APP_LOG_LEVEL_INFO, "settings: HandsColor = 0x%06lx",
            (unsigned long)hex);
  }

  t = dict_find(iter, MESSAGE_KEY_HandsOutlineColor);
  if (t) {
    uint32_t hex = (uint32_t)t->value->int32;
    s_hands_outline_color = GColorFromHEX(hex);
    APP_LOG(APP_LOG_LEVEL_INFO, "settings: HandsOutlineColor = 0x%06lx",
            (unsigned long)hex);
  }

  t = dict_find(iter, MESSAGE_KEY_HandsThickness);
  if (t) {
    uint8_t style = 1;
    if (t->type == TUPLE_CSTRING && t->length > 0) {
      style = (uint8_t)atoi(t->value->cstring);
    } else if (t->type == TUPLE_INT) {
      style = (uint8_t)t->value->int32;
    }
    if (style > 2) style = 1;
    s_hands_thickness = style;
    APP_LOG(APP_LOG_LEVEL_INFO, "settings: HandsThickness = %d",
            (int)s_hands_thickness);
  }

  bool changed_text_style = false;
  t = dict_find(iter, MESSAGE_KEY_TextColor);
  if (t) {
    uint32_t hex = (uint32_t)t->value->int32;
    s_text_color = GColorFromHEX(hex);
    changed_text_style = true;
    APP_LOG(APP_LOG_LEVEL_INFO, "settings: TextColor = 0x%06lx",
            (unsigned long)hex);
  }

  t = dict_find(iter, MESSAGE_KEY_TextOutline);
  if (t) {
    s_text_outline_enabled = (t->value->int8 != 0);
    changed_text_style = true;
    APP_LOG(APP_LOG_LEVEL_INFO, "settings: TextOutline = %d",
            (int)s_text_outline_enabled);
  }

  t = dict_find(iter, MESSAGE_KEY_TextOutlineColor);
  if (t) {
    uint32_t hex = (uint32_t)t->value->int32;
    s_text_outline_color = GColorFromHEX(hex);
    changed_text_style = true;
    APP_LOG(APP_LOG_LEVEL_INFO, "settings: TextOutlineColor = 0x%06lx",
            (unsigned long)hex);
  }

  // Reset Tama if the user toggled it on. Wipes EEPROM + resets the CPU
  // so the Tama firmware boots from a blank slate (first-time setup
  // screen / egg select). The toggle itself isn't persisted — it only
  // takes effect for the current save cycle, then resets to OFF in Clay
  // memory (because we never write it back).
  t = dict_find(iter, MESSAGE_KEY_ResetTama);
  if (t && t->value->int8 != 0) {
    APP_LOG(APP_LOG_LEVEL_INFO, "settings: ResetTama -> wiping state");
    tamago_eeprom_wipe();
    tamago_reset();
  }

  if (changed_palette) {
    update_palette_from_color(s_tama_pixel_color);
  }
  if (changed_text_style) {
    apply_text_style();
  }

  save_settings();

  // Trigger a redraw so the new look takes effect immediately.
  if (s_bg_layer)        layer_mark_dirty(s_bg_layer);
  if (s_tama_bg_layer)   layer_mark_dirty(s_tama_bg_layer);
  if (s_tama_layer)      layer_mark_dirty(s_tama_layer);
  if (s_icons_top_layer) layer_mark_dirty(s_icons_top_layer);
  if (s_icons_bot_layer) layer_mark_dirty(s_icons_bot_layer);
  if (s_hands_layer)     layer_mark_dirty(s_hands_layer);
}

// ----- App init / deinit ----

// Periodic RTC sync — adaptive interval. The Tama firmware overwrites
// our initial-sync write during its boot (settles to 09:00:00 default).
// So the first periodic check needs to fire quickly to catch that and
// re-sync. After we've seen a small drift once, the clock is stable
// and we can fall back to 15 minutes.
#define RTC_SYNC_INTERVAL_FAST_MS  (60 * 1000)        // first checks
#define RTC_SYNC_INTERVAL_SLOW_MS  (15 * 60 * 1000)   // after we settle
static AppTimer *s_rtc_sync_timer;
static bool      s_rtc_settled = false;   // true once drift was small once

static void rtc_sync_tick(void *data)
{
  s_rtc_sync_timer = NULL;
  if (!s_running) return;
  int32_t abs_drift = tamago_rtc_periodic_check();
  // Once we've seen drift stay within the small-drift band, switch to
  // the slow interval. If we ever see a big drift again (e.g. user
  // pressed Reset), we fall back to fast checks.
  if (abs_drift <= TAMAGO_RTC_DRIFT_THRESHOLD_S) {
    s_rtc_settled = true;
  } else {
    s_rtc_settled = false;
  }
  uint32_t next_ms = s_rtc_settled ? RTC_SYNC_INTERVAL_SLOW_MS
                                   : RTC_SYNC_INTERVAL_FAST_MS;
  s_rtc_sync_timer = app_timer_register(next_ms, rtc_sync_tick, NULL);
}

static void app_init(void)
{
  load_settings();

  if (!tamago_init()) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "tamago_init failed -- aborting");
    return;
  }

  // Sync the Tama's internal clock to the Pebble RTC right away. The
  // initial value will only "stick" once the Tama firmware finishes its
  // own boot sequence and reaches the running state — but doing it here
  // means we overwrite whatever stale value was loaded from EEPROM and
  // the first running tick already has the right time. The periodic
  // check below will catch any drift / boot-init overwrite.
  tamago_rtc_initial_sync();

  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = window_load,
    .unload = window_unload,
  });
  window_set_click_config_provider(s_window, click_config_provider);
  window_stack_push(s_window, true);

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
  battery_state_service_subscribe(battery_handler);

  // Clay sends settings via AppMessage whenever the user hits Save.
  app_message_register_inbox_received(inbox_received_handler);
  app_message_open(256, 64);

  s_running = true;
  s_step_timer = app_timer_register(EMU_FRAME_MS, step_tick, NULL);
  // First RTC check uses the fast interval to catch the boot-clobber.
  s_rtc_sync_timer = app_timer_register(RTC_SYNC_INTERVAL_FAST_MS,
                                        rtc_sync_tick, NULL);
}

static void app_deinit(void)
{
  s_running = false;
  if (s_step_timer)     { app_timer_cancel(s_step_timer);     s_step_timer = NULL; }
  if (s_rtc_sync_timer) { app_timer_cancel(s_rtc_sync_timer); s_rtc_sync_timer = NULL; }
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
