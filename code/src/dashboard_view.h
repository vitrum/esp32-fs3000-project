#pragma once

#include "dashboard_layout.h"
#include "sd_activity.h"
#include <lvgl.h>
#include <cmath>
#include <cstdio>

extern const lv_font_t lv_font_noto_sans_sc_14;

class DashboardView {
public:
  // 数值区几何：从行框左侧 ValueLeft 起，可用宽度 ValueWidth。
  static constexpr int ValueWidth = 136;
  static constexpr int ValueLeft = 80;
  static constexpr unsigned WindRow = 1;   // 风速行 id（整数段 + 小数段）
  static constexpr int kTextSlack = 1;     // 标签框相对文字宽度的余量，防 CLIP 取整裁字

  void create(lv_obj_t *screen, lv_event_cb_t sdCallback, lv_event_cb_t settingsCallback) {
    auto *bar = lv_obj_create(screen);
    lv_obj_set_size(bar, 240, 28);
    lv_obj_set_pos(bar, 0, 0);
    plain(bar);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
    gps = text(bar, 2, 4, 20, 20, &lv_font_jetbrains_mono_16);
    lv_label_set_text(gps, LV_SYMBOL_GPS);
    satellites = text(bar, 22, 4, 24, 20, &lv_font_jetbrains_mono_16);
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
    sdIcon = text(bar, 58, 4, 20, 20, &lv_font_jetbrains_mono_16);
    lv_label_set_text(sdIcon, LV_SYMBOL_SAVE);
    dot_ = lv_obj_create(bar);
    lv_obj_set_pos(dot_, 82, 11);
    lv_obj_set_size(dot_, 6, 6);
    plain(dot_);
    lv_obj_set_style_radius(dot_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dot_, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dot_, lv_color_make(239, 68, 68), 0);
    settingsButton = hotButton(screen, 94, settingsCallback);
    auto *gear = text(bar, 106, 4, 20, 20, &lv_font_jetbrains_mono_16);
    lv_label_set_text(gear, LV_SYMBOL_SETTINGS);
    clock = text(bar, 140, 4, 98, 20, &lv_font_jetbrains_mono_16);
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
      row.title = text(row.box, 0, 0, 72, 18, &lv_font_jetbrains_mono_14);
      row.unit = text(row.box, 0, 18, 72, 18, &lv_font_jetbrains_mono_14);
      if (index == 0) {
        row.detail = text(row.box, 0, 36, 72, 18, &lv_font_jetbrains_mono_14);
        lv_obj_set_style_text_color(row.detail, lv_color_make(100, 116, 139), 0);
      }
      row.value = text(row.box, 80, 0, 136, 36, &lv_font_jetbrains_mono_20);
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
      lv_obj_set_style_text_font(rows_[index].title, chinese ? &lv_font_noto_sans_sc_14 : &lv_font_jetbrains_mono_14, 0);
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
    // 风速行（id=1）在 value() 内部走拆段渲染：整数随字号放大，小数固定 14px。
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
  struct Row { DashboardView *owner = nullptr; unsigned id = 0; lv_obj_t *box = nullptr, *title = nullptr, *unit = nullptr, *value = nullptr, *line = nullptr, *detail = nullptr, *frac = nullptr; };
  // ---------- 数值字号选择（所有行共用同一套规则） ----------
  // 目标字号来自该行的 level；放不下时沿候选链逐级回退。
  // budget 是这一段文本可用的水平像素宽度。
  const lv_font_t *pickFont(unsigned id, const char *textValue, int budget) const {
    const lv_font_t *selected = font(layout_.level(id));
    const lv_font_t *candidates[] = {selected, &lv_font_jetbrains_mono_40, &lv_font_jetbrains_mono_28,
                                     &lv_font_jetbrains_mono_20, &lv_font_jetbrains_mono_14};
    for (const auto *candidate : candidates) {
      if (candidate->line_height > selected->line_height) continue;
      lv_point_t size;
      lv_txt_get_size(&size, textValue, candidate, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
      if (size.x <= budget) return candidate;
    }
    return &lv_font_jetbrains_mono_14;
  }
  static int textWidth(const char *textValue, const lv_font_t *font) {
    lv_point_t size;
    lv_txt_get_size(&size, textValue, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
  }

  // ---------- 风速行：整数段 + 小数段 ----------
  // 布局：整数 + 小数作为一个整体，在数值区内水平居中，垂直居中；
  // 小数段紧跟整数段右侧，互不重叠。
  //
  //    |<---------------- ValueWidth (136) -------------->|
  //              [ 22 ][.5]          <- 整体居中
  //
  // 注意：数值区只有 136px，不能把小数叠在整数上，否则必然重叠。
  void renderWindSplit(const char *textValue) {
    auto &row = rows_[WindRow];
    if (!row.box || !row.value) return;
    if (!row.frac) {
      row.frac = text(row.box, 0, 0, ValueWidth, 36, &lv_font_jetbrains_mono_14);
      lv_obj_clear_flag(row.frac, LV_OBJ_FLAG_CLICKABLE);
    }
    const char *point = strchr(textValue, '.');
    char wholeText[32];
    if (point) {
      const size_t wholeLength = static_cast<size_t>(point - textValue);
      if (wholeLength >= sizeof(wholeText)) return;
      memcpy(wholeText, textValue, wholeLength);
      wholeText[wholeLength] = '\0';
      if (strcmp(lv_label_get_text(row.frac), point)) lv_label_set_text(row.frac, point);
      lv_obj_clear_flag(row.frac, LV_OBJ_FLAG_HIDDEN);
    } else {
      snprintf(wholeText, sizeof(wholeText), "%s", textValue);
      lv_obj_add_flag(row.frac, LV_OBJ_FLAG_HIDDEN);
    }
    if (strcmp(lv_label_get_text(row.value), wholeText)) lv_label_set_text(row.value, wholeText);
    layoutWindSplit();
  }

  // 根据当前行高、整数段与小数的实际字体重新排版。
  void layoutWindSplit() {
    auto &row = rows_[WindRow];
    if (!row.value || !row.frac) return;
    const int rowHeight = lv_obj_get_height(row.box);
    const bool hasFraction = !lv_obj_has_flag(row.frac, LV_OBJ_FLAG_HIDDEN);
    const int fractionWidth = hasFraction
        ? textWidth(lv_label_get_text(row.frac), &lv_font_jetbrains_mono_14) : 0;
    const char *wholeText = lv_label_get_text(row.value);

    // 整数段的字号与其它行同一规则，宽度预算扣除小数段（两者是并列的，不是叠加）。
    const lv_font_t *wholeFont = pickFont(WindRow, wholeText, ValueWidth - fractionWidth);
    lv_obj_set_style_text_font(row.value, wholeFont, 0);

    // 与其它行保持一致：整块右对齐到数值区右边缘（ValueLeft + ValueWidth）。
    // 普通行的标签框宽 ValueWidth 且 TEXT_ALIGN_RIGHT，右缘恒为 ValueLeft + ValueWidth；
    // 风速这里让「小数段」的右缘贴到同一条线上，整数段紧贴其左侧。
    const int wholeTextWidth = textWidth(wholeText, wholeFont);   // 等宽字体下即 adv 之和
    const int rightEdge = ValueLeft + ValueWidth;
    // 整数框多出的 kTextSlack 会把小数段整个右推，先把它算进总宽里。
    const int blockLeft = rightEdge - wholeTextWidth - kTextSlack - fractionWidth;
    const int wholeTop = (rowHeight - wholeFont->line_height) / 2;

    // 整数段：标签框按自身宽度摆放，右对齐后自然紧贴小数段。
    // 宽度留 1px 余量，避免 CLIP 模式因取整把末位数字裁掉。
    lv_obj_set_pos(row.value, blockLeft, wholeTop);
    lv_obj_set_size(row.value, wholeTextWidth + kTextSlack, wholeFont->line_height);
    lv_obj_set_style_text_align(row.value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(row.frac, lv_obj_get_style_text_color(row.value, 0), 0);

    if (!hasFraction) return;

    // 小数段：紧接整数段右侧，垂直居中于整数段，右缘落在数值区右边缘。
    const int fractionHeight = lv_font_jetbrains_mono_14.line_height;
    const int wholeCenterY = wholeTop + wholeFont->line_height / 2;
    lv_obj_set_pos(row.frac, blockLeft + wholeTextWidth + kTextSlack,
                   wholeCenterY - fractionHeight / 2);
    lv_obj_set_size(row.frac, fractionWidth, fractionHeight);
    lv_obj_set_style_text_align(row.frac, LV_TEXT_ALIGN_LEFT, 0);
  }

  void renderAero() {
    if (!rows_[0].unit) return;
    char textValue[32];
    if (std::isfinite(cda_) && cda_ >= 0) snprintf(textValue, sizeof(textValue), "CdA %.2g", cda_);
    else snprintf(textValue, sizeof(textValue), "CdA --");
    lv_point_t size;
    lv_txt_get_size(&size, textValue, &lv_font_jetbrains_mono_14, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (size.x > 72) snprintf(textValue, sizeof(textValue), cda_ < 10000 ? "CdA %.0f" : "CdA >999", cda_);
    lv_label_set_text(rows_[0].unit, textValue);
    const char *english[] = {"NO DATA", "UPRIGHT", "DROPS", "AERO", "AGGR.", "LOW", "RACE", "TT"};
    const char *chinese[] = {"等待数据", "直立", "低位下把", "气动", "激进", "极致气动", "竞赛姿态", "精英姿态"};
    lv_label_set_text(rows_[0].detail, chinese_ ? chinese[zone_] : english[zone_]);
    lv_obj_set_style_text_font(rows_[0].detail, chinese_ ? &lv_font_noto_sans_sc_14 : &lv_font_jetbrains_mono_14, 0);
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
    if (level == 4) return &lv_font_jetbrains_mono_48;
    if (level == 3) return &lv_font_jetbrains_mono_40;
    if (level == 2) return &lv_font_jetbrains_mono_28;
    return &lv_font_jetbrains_mono_20;
  }
  void value(unsigned id, const char *textValue) {
    // 风速行拆成「整数 + 小数」两段，整数字号规则与其它行一致。
    if (id == WindRow) { renderWindSplit(textValue); return; }
    if (strcmp(lv_label_get_text(rows_[id].value), textValue)) lv_label_set_text(rows_[id].value, textValue);
    lv_obj_set_style_text_font(rows_[id].value, pickFont(id, textValue, ValueWidth), 0);
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
      // 先按该行 max 字号占位，具体字号由 value() 按内容与预算决定。
      const int textHeight = font(layout_.level(id))->line_height;
      lv_obj_set_pos(row.value, ValueLeft, (height - textHeight) / 2);
      lv_obj_set_size(row.value, ValueWidth, textHeight);
      value(id, lv_label_get_text(row.value));
      // 风速行的整数段字号可能被降级，需要按最终字号重新居中。
      if (id == WindRow) layoutWindSplit();
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
      const bool dragging = view.layout_.motion(point.y, lv_tick_get(), point.x);
      const bool ready = view.layout_.dragReady(lv_tick_get());
      const unsigned target = dragging ? view.layout_.id(view.layout_.positionAt(point.y)) : row->id;
      for (auto &candidate : view.rows_) {
        const bool active = (ready && candidate.id == row->id) || (dragging && candidate.id == target);
        lv_obj_set_style_bg_opa(candidate.box, active ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(candidate.box, candidate.id == row->id ? lv_color_make(219, 234, 254) : lv_color_make(187, 247, 208), 0);
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