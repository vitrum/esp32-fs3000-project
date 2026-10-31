#pragma once
#include <lvgl.h>
#include <cmath>
#include <cstdio>

extern const lv_font_t lv_font_noto_sans_sc_14;

class AeroZoneView {
public:
  void create(lv_obj_t *screen) {
    parent_ = lv_obj_create(screen);
    lv_obj_set_size(parent_, 216, 88);
    lv_obj_set_pos(parent_, 12, 43);
    lv_obj_clear_flag(parent_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(parent_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_all(parent_, 0, 0);
    lv_obj_set_style_radius(parent_, 0, 0);
    lv_obj_set_style_border_width(parent_, 0, 0);
    lv_obj_set_style_bg_opa(parent_, LV_OPA_TRANSP, 0);
    title_ = label(0, 0, 104, 22, &lv_font_jetbrains_mono_14, false);
    description_ = label(0, 29, 104, 22, &lv_font_jetbrains_mono_14, false);
    zone_ = label(116, 12, 100, 50, &lv_font_jetbrains_mono_40, true);
    unit_ = label(0, 70, 104, 18, &lv_font_jetbrains_mono_14, false);
    value_ = label(116, 70, 100, 18, &lv_font_jetbrains_mono_14, true);
    lv_label_set_text(unit_, "CdA / m2");
    lv_obj_set_style_text_color(zone_, lv_color_make(37, 99, 235), 0);
    lv_obj_t *line = lv_obj_create(parent_);
    lv_obj_set_pos(line, 0, 64);
    lv_obj_set_size(line, 216, 1);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 0, 0);
    lv_obj_set_style_bg_color(line, lv_color_make(203, 213, 225), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    render();
  }
  void update(float cda, unsigned zone) { cda_ = cda; zoneValue_ = zone <= 7 ? zone : 0; render(); }
  void setLanguage(bool chinese) { chinese_ = chinese; render(); }
  lv_obj_t *parent() const { return parent_; }
  lv_obj_t *title() const { return title_; }
  lv_obj_t *description() const { return description_; }
  lv_obj_t *zone() const { return zone_; }
  lv_obj_t *unit() const { return unit_; }
  lv_obj_t *value() const { return value_; }
private:
  lv_obj_t *label(int x, int y, int width, int height, const lv_font_t *font, bool right) {
    lv_obj_t *result = lv_label_create(parent_);
    lv_obj_set_pos(result, x, y);
    lv_obj_set_size(result, width, height);
    lv_label_set_long_mode(result, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_pad_all(result, 0, 0);
    lv_obj_set_style_text_font(result, font, 0);
    lv_obj_set_style_text_align(result, right ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(result, lv_color_make(100, 116, 139), 0);
    return result;
  }
  void render() {
    if (!parent_) return;
    static const char *const english[] = {"NO DATA", "UPRIGHT", "DROPS", "AERO", "AGGRESSIVE", "VERY AERO", "RACE TT", "ELITE TT"};
    static const char *const chinese[] = {"等待数据", "直立", "低位下把", "气动", "激进", "极致气动", "竞赛姿态", "精英姿态"};
    const lv_font_t *font = chinese_ ? &lv_font_noto_sans_sc_14 : &lv_font_jetbrains_mono_14;
    lv_obj_set_style_text_font(title_, font, 0);
    lv_obj_set_style_text_font(description_, font, 0);
    lv_label_set_text(title_, chinese_ ? "气动区间" : "AERO ZONE");
    lv_label_set_text(description_, chinese_ ? chinese[zoneValue_] : english[zoneValue_]);
    char text[32];
    if (zoneValue_) snprintf(text, sizeof(text), "Z%u", zoneValue_);
    else snprintf(text, sizeof(text), "Z--");
    lv_label_set_text(zone_, text);
    if (std::isfinite(cda_) && cda_ >= 0) snprintf(text, sizeof(text), "%.2f", cda_);
    else snprintf(text, sizeof(text), "--");
    lv_label_set_text(value_, text);
  }
  lv_obj_t *parent_ = nullptr, *title_ = nullptr, *description_ = nullptr;
  lv_obj_t *zone_ = nullptr, *unit_ = nullptr, *value_ = nullptr;
  float cda_ = -1;
  unsigned zoneValue_ = 0;
  bool chinese_ = false;
};