#include "ui.h"
#include <lvgl.h>
#include "fonts.h"

// Same look as the app's meters: white blocks, soft white labels on black.
#define C_FG 0xFFFFFF
#define OPA_SOFT 140  // rgba(255,255,255,0.55)

#define STATS_Y 414
#define ROW_H 30
#define BLOCK_W 18
#define BLOCK_H 10
#define BLOCK_GAP 5
#define LABEL_W 100

static lv_obj_t *panel, *soundIcon;
static lv_obj_t *blocks[2][5];
static bool shown = false;

static void row(int index, const char *title) {
  int width = LABEL_W + 5 * BLOCK_W + 4 * BLOCK_GAP;
  int x = (SCREEN_W - width) / 2, y = index * ROW_H;
  lv_obj_t *t = lv_label_create(panel);
  lv_obj_set_style_text_font(t, &font_medium_14, 0);
  lv_obj_set_style_text_color(t, lv_color_hex(C_FG), 0);
  lv_obj_set_style_text_opa(t, OPA_SOFT, 0);
  lv_obj_set_style_text_letter_space(t, 1, 0);
  lv_obj_set_pos(t, x, y);
  lv_label_set_text(t, title);
  for (int i = 0; i < 5; i++) {
    lv_obj_t *b = lv_obj_create(panel);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, BLOCK_W, BLOCK_H);
    lv_obj_set_pos(b, x + LABEL_W + i * (BLOCK_W + BLOCK_GAP), y + 4);
    lv_obj_set_style_radius(b, 2, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(C_FG), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_FG), 0);
    blocks[index][i] = b;
  }
}

void uiBegin() {
  lv_obj_t *scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  panel = lv_obj_create(scr);
  lv_obj_remove_style_all(panel);
  lv_obj_set_size(panel, SCREEN_W, 2 * ROW_H);
  lv_obj_set_pos(panel, 0, STATS_Y);
  lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_opa(panel, LV_OPA_TRANSP, 0);
  row(0, "SAZIETÀ");
  row(1, "VITALITÀ");

  soundIcon = lv_label_create(scr);
  lv_obj_set_style_text_font(soundIcon, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(soundIcon, lv_color_hex(C_FG), 0);
  lv_obj_set_width(soundIcon, SCREEN_W);
  lv_obj_set_style_text_align(soundIcon, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(soundIcon, 0, STATS_Y + 12);
  lv_obj_set_style_opa(soundIcon, LV_OPA_TRANSP, 0);
}

void uiShowSound(bool on) {
  lv_label_set_text(soundIcon, on ? LV_SYMBOL_VOLUME_MAX : LV_SYMBOL_MUTE);
  // Shown at once, then faded out (two fades on one object would cancel each other).
  lv_anim_delete(soundIcon, nullptr);
  lv_obj_set_style_opa(soundIcon, LV_OPA_COVER, 0);
  lv_obj_fade_out(soundIcon, 400, 1400);
}

void uiSetMeters(int satiety, int vitality) {
  int values[2] = {satiety, vitality};
  for (int m = 0; m < 2; m++)
    for (int i = 0; i < 5; i++) {
      lv_obj_set_style_bg_opa(blocks[m][i], i < values[m] ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
      lv_obj_set_style_border_opa(blocks[m][i], i < values[m] ? LV_OPA_COVER : OPA_SOFT, 0);
    }
}

void uiShowStats(bool show) {
  if (show == shown) return;
  shown = show;
  if (show) lv_obj_fade_in(panel, 250, 0);
  else lv_obj_fade_out(panel, 400, 0);
}
