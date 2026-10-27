#pragma once

#include "dashboard_layout.h"
#include "sd_activity.h"
#include <lvgl.h>
#include <cmath>
#include <cstdio>

extern const lv_font_t lv_font_noto_sans_sc_14;

class DashboardView {
public:
  void create(lv_obj_t *screen, lv_event_cb_t sdCallback, lv_event_cb_t settingsCallback) {
    auto *bar = lv_obj_create(screen);
    lv_obj_set_size(bar, 240, 28);
    lv_obj_set_pos(bar, 0, 0);
    plain(bar);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
    gps = text(bar, 2, 4, 20, 20, &lv_font_montserrat_16);
    lv_label_set_text(gps, LV_SYMBOL_GPS);
    satellites = text(bar, 22, 4, 24, 20, &lv_font_montserrat_16);
    lv_label_set_text(satellites, "--");
    spinner = lv_spinner_create(bar, 1000, 60);
    lv_obj_set_pos(spinner, 25, 6);
    lv_obj_set_size(spinner, 16, 16);
    lv_obj_set_style_arc_width(spinner, 2, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spinner, 2, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(spinner, lv_color_white(), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(spinner, lv_color_make(80, 80, 80), LV_PART_MAIN);
    lv_obj_add_flag(spinner, LV_OBJ_FLAG_HIDDEN);
    sdButton = hotButton(screen, 48, sdCallback);
    sdIcon = text(bar, 58, 4, 20, 20, &lv_font_montserrat_16);
    lv_label_set_text(sdIcon, LV_SYMBOL_SAVE);
    dot_ = lv_obj_create(bar);
    lv_obj_set_pos(dot_, 82, 11);
    lv_obj_set_size(dot_, 6, 6);
    plain(dot_);
    lv_obj_set_style_radius(dot_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dot_, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dot_, lv_color_make(239, 68, 68), 0);
    settingsButton = hotButton(screen, 94, settingsCallback);
    auto *gear = text(bar, 106, 4, 20, 20, &lv_font_montserrat_16);
    lv_label_set_text(gear, LV_SYMBOL_SETTINGS);
    clock = text(bar, 140, 4, 98, 20, &lv_font_montserrat_16);
    lv_obj_set_style_text_align(clock, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(clock, "--:--:--");
    for (unsigned index = 0; index < DashboardLayout::Count; ++index) {
      auto &row = rows_[index];
      row.owner = this;
      row.id = index;
      row.box = lv_obj_create(screen);
      lv_obj_set_width(row.box, 216);
      plain(row.box);
      lv_obj_set_style_bg_opa(row.box, LV_OPA_TRANSP, 0);
      lv_obj_clear_flag(row.box, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_add_flag(row.box, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_flag(row.box, LV_OBJ_FLAG_PRESS_LOCK);
      lv_obj_add_event_cb(row.box, event, LV_EVENT_ALL, &row);
      row.title = text(row.box, 0, 0, 72, 18, &lv_font_montserrat_14);
      row.unit = text(row.box, 0, 18, 72, 18, &lv_font_montserrat_14);
      if (index == 0) {
        row.detail = text(row.box, 0, 36, 72, 18, &lv_font_montserrat_14);
        lv_obj_set_style_text_color(row.detail, lv_color_make(100, 116, 139), 0);
      }
      row.value = text(row.box, 80, 0, 136, 36, &lv_font_montserrat_20);
      lv_obj_set_style_text_align(row.value, LV_TEXT_ALIGN_RIGHT, 0);
      lv_label_set_text(row.value, "--");
      row.line = lv_obj_create(row.box);
      plain(row.line);
      lv_obj_set_size(row.line, 216, 1);
      lv_obj_set_style_bg_opa(row.line, LV_OPA_COVER, 0);
      lv_obj_set_style_bg_color(row.line, lv_color_make(203, 213, 225), 0);
      lv_obj_clear_flag(row.line, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_style_text_color(row.value, index == 0 || index == 2 ? lv_color_make(37, 99, 235) : lv_color_make(23, 32, 51), 0);
      lv_obj_set_style_text_color(row.title, lv_color_make(71, 85, 105), 0);
      lv_obj_set_style_text_color(row.unit, lv_color_make(100, 116, 139), 0);
    }
    setLanguage(false);
    reflow();
  }
  void setLanguage(bool chinese) {
    chinese_ = chinese;
    const char *english[] = {"ZONE", "WIND", "POWER", "CADENCE", "HEART"};
    const char *translated[] = {"气动区间", "风速", "功率", "踏频", "心率"};
    const char *units[] = {"CdA / m2", "km/h", "W", "rpm", "bpm"};
    for (unsigned index = 0; index < 5; ++index) {
      if (!rows_[index].box) continue;
      lv_label_set_text(rows_[index].title, chinese ? translated[index] : english[index]);
      lv_obj_set_style_text_font(rows_[index].title, chinese ? &lv_font_noto_sans_sc_14 : &lv_font_montserrat_14, 0);
      lv_label_set_text(rows_[index].unit, units[index]);
    }
    renderAero();
    reflow();
  }
  void update(float cda, unsigned zone, float wind, bool windValid, float power, float cadence) {
    cda_ = cda;
    zone_ = zone <= 7 ? zone : 0;
    char buffer[32];
    if (zone) snprintf(buffer, sizeof(buffer), "Z%u", zone); else snprintf(buffer, sizeof(buffer), "Z--");
    value(0, buffer);
    renderAero();
    if (windValid) snprintf(buffer, sizeof(buffer), "%.1f", wind * 3.6f); else snprintf(buffer, sizeof(buffer), "--");
    value(1, buffer);
    snprintf(buffer, sizeof(buffer), "%.0f", power); value(2, buffer);
    snprintf(buffer, sizeof(buffer), "%.0f", cadence); value(3, buffer);
  }
  void heart(uint16_t bpm, bool valid) {
    char buffer[16];
    if (valid) snprintf(buffer, sizeof(buffer), "%u", bpm); else snprintf(buffer, sizeof(buffer), "--");
    value(4, buffer);
  }
  void storage(SdActivity::State state, uint32_t now) {
    const bool writing = state == SdActivity::State::Writing;
    const auto color = state == SdActivity::State::Absent ? lv_color_make(148, 163, 184) :
        state == SdActivity::State::Ready ? lv_color_make(34, 197, 94) : lv_color_make(239, 68, 68);
    lv_obj_set_style_text_color(sdIcon, color, 0);
    lv_label_set_text(sdIcon, state == SdActivity::State::Error ? LV_SYMBOL_WARNING : LV_SYMBOL_SAVE);
    if (writing && ((now / 250) & 1)) lv_obj_clear_flag(dot_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(dot_, LV_OBJ_FLAG_HIDDEN);
  }
  bool load(const DashboardLayout::Settings &settings) { const bool valid = layout_.load(settings); reflow(); return valid; }
  bool takeSettings(DashboardLayout::Settings &settings, bool force = false) {
    if (!dirty_ || (!force && lv_tick_get() - changedAt_ < 800)) return false;
    settings = layout_.settings();
    for (unsigned index = 0; index < 5; ++index) settings.levels[index] = layout_.level(index);
    dirty_ = false;
    return true;
  }
  void retrySave() { dirty_ = true; changedAt_ = lv_tick_get(); }
  void reset() { layout_.reset(); reflow(); retrySave(); }
  DashboardLayout &layout() { return layout_; }
  lv_obj_t *row(unsigned id) const { return rows_[id].box; }
  lv_obj_t *title(unsigned id) const { return rows_[id].title; }
  lv_obj_t *reading(unsigned id) const { return rows_[id].value; }
  lv_obj_t *gps = nullptr, *satellites = nullptr, *spinner = nullptr, *clock = nullptr;
  lv_obj_t *sdButton = nullptr, *sdIcon = nullptr, *settingsButton = nullptr;

private:
  struct Row { DashboardView *owner = nullptr; unsigned id = 0; lv_obj_t *box = nullptr, *title = nullptr, *unit = nullptr, *value = nullptr, *line = nullptr, *detail = nullptr; };
  void renderAero() {
    if (!rows_[0].unit) return;
    char textValue[32];
    if (std::isfinite(cda_) && cda_ >= 0) snprintf(textValue, sizeof(textValue), "CdA %.2g", cda_);
    else snprintf(textValue, sizeof(textValue), "CdA --");
    lv_point_t size;
    lv_txt_get_size(&size, textValue, &lv_font_montserrat_14, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (size.x > 72) snprintf(textValue, sizeof(textValue), cda_ < 10000 ? "CdA %.0f" : "CdA >999", cda_);
    lv_label_set_text(rows_[0].unit, textValue);
    const char *english[] = {"NO DATA", "UPRIGHT", "DROPS", "AERO", "AGGR.", "LOW", "RACE", "TT"};
    const char *chinese[] = {"等待数据", "直立", "低位下把", "气动", "激进", "极致气动", "竞赛姿态", "精英姿态"};
    lv_label_set_text(rows_[0].detail, chinese_ ? chinese[zone_] : english[zone_]);
    lv_obj_set_style_text_font(rows_[0].detail, chinese_ ? &lv_font_noto_sans_sc_14 : &lv_font_montserrat_14, 0);
  }
  static void plain(lv_obj_t *object) {
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_radius(object, 0, 0);
    lv_obj_set_style_shadow_width(object, 0, 0);
  }
  static lv_obj_t *text(lv_obj_t *parent, int left, int top, int width, int height, const lv_font_t *font) {
    auto *label = lv_label_create(parent);
    lv_obj_set_pos(label, left, top); lv_obj_set_size(label, width, height);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    return label;
  }
  static lv_obj_t *hotButton(lv_obj_t *screen, int left, lv_event_cb_t callback) {
    auto *button = lv_btn_create(screen);
    lv_obj_set_pos(button, left, 0); lv_obj_set_size(button, 44, 44);
    plain(button);
    lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, nullptr);
    return button;
  }
  const lv_font_t *font(unsigned level) const {
    if (level == 4) return &lv_font_montserrat_48;
    if (level == 3) return &lv_font_montserrat_40;
    if (level == 2) return &lv_font_montserrat_28;
    return &lv_font_montserrat_20;
  }
  void value(unsigned id, const char *textValue) {
    if (strcmp(lv_label_get_text(rows_[id].value), textValue)) lv_label_set_text(rows_[id].value, textValue);
    const lv_font_t *selected = font(layout_.level(id));
    const lv_font_t *candidates[] = {selected, &lv_font_montserrat_40, &lv_font_montserrat_28, &lv_font_montserrat_20, &lv_font_montserrat_14};
    for (const auto *candidate : candidates) {
      if (candidate->line_height > selected->line_height) continue;
      lv_point_t size;
      lv_txt_get_size(&size, textValue, candidate, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
      if (size.x <= 136) { selected = candidate; break; }
    }
    lv_obj_set_style_text_font(rows_[id].value, selected, 0);
  }
  void reflow() {
    for (unsigned position = 0; position < 5; ++position) {
      const unsigned id = layout_.id(position);
      auto &row = rows_[id];
      if (!row.box) continue;
      const int height = layout_.height(id);
      lv_obj_set_pos(row.box, 12, layout_.top(position));
      lv_obj_set_height(row.box, height);
      lv_obj_set_pos(row.line, 0, height - 1);
      if (row.detail) {
        if (layout_.level(id) >= 3) lv_obj_clear_flag(row.detail, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(row.detail, LV_OBJ_FLAG_HIDDEN);
      }
      const int textHeight = font(layout_.level(id))->line_height;
      lv_obj_set_pos(row.value, 80, (height - textHeight) / 2);
      lv_obj_set_height(row.value, textHeight);
      value(id, lv_label_get_text(row.value));
    }
  }
  static void event(lv_event_t *event) {
    auto *row = static_cast<Row *>(lv_event_get_user_data(event));
    auto &view = *row->owner;
    const auto code = lv_event_get_code(event);
    auto *input = lv_indev_get_act();
    if (!input) return;
    lv_point_t point;
    lv_indev_get_point(input, &point);
    if (code == LV_EVENT_PRESSED) {
      unsigned position = 0;
      while (position < 5 && view.layout_.id(position) != row->id) ++position;
      view.layout_.press(position, point.y, lv_tick_get(), point.x);
    } else if (code == LV_EVENT_PRESSING) {
      if (view.layout_.motion(point.y, lv_tick_get(), point.x)) {
        const unsigned target = view.layout_.id(view.layout_.positionAt(point.y));
        for (auto &candidate : view.rows_) {
          lv_obj_set_style_bg_opa(candidate.box, candidate.id == target || candidate.id == row->id ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
          lv_obj_set_style_bg_color(candidate.box, candidate.id == target ? lv_color_make(187, 247, 208) : lv_color_make(219, 234, 254), 0);
        }
      }
    } else if (code == LV_EVENT_RELEASED) {
      if (view.layout_.release(point.y, lv_tick_get(), point.x)) { view.reflow(); view.retrySave(); }
      for (auto &candidate : view.rows_) lv_obj_set_style_bg_opa(candidate.box, LV_OPA_TRANSP, 0);
    } else if (code == LV_EVENT_PRESS_LOST) {
      view.layout_.cancel();
      for (auto &candidate : view.rows_) lv_obj_set_style_bg_opa(candidate.box, LV_OPA_TRANSP, 0);
    }
  }
  Row rows_[5];
  DashboardLayout layout_;
  lv_obj_t *dot_ = nullptr;
  bool chinese_ = false, dirty_ = false;
  uint32_t changedAt_ = 0;
  float cda_ = -1;
  unsigned zone_ = 0;
};