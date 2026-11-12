#pragma once

#include <lvgl.h>
#include <cstring>
#include <cstdio>
#include <initializer_list>
#include "jetbrains_mono_latin.h"
#include "rider_posture.h"

LV_FONT_DECLARE(lv_font_noto_sans_sc_14);

class OnlineDevicesView {
public:
  static constexpr unsigned kCount = 6;
  void create(lv_obj_t *panel, const char *version) {
    version_ = lv_label_create(panel);
    lv_obj_set_pos(version_, 8, 54);
    lv_obj_set_size(version_, 224, 18);
    lv_obj_set_style_text_font(version_, &lv_font_jetbrains_mono_14, 0);
    lv_obj_set_style_text_color(version_, lv_color_make(100, 116, 139), 0);
    lv_label_set_text_fmt(version_, "V %s", version);
    list_ = lv_obj_create(panel);
    lv_obj_set_pos(list_, 0, 74);
    lv_obj_set_size(list_, 240, 246);
    lv_obj_set_style_pad_all(list_, 0, 0);
    lv_obj_set_style_border_width(list_, 0, 0);
    lv_obj_set_style_radius(list_, 0, 0);
    lv_obj_set_style_bg_opa(list_, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(list_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(list_, LV_SCROLLBAR_MODE_OFF);
    for (unsigned index = 0; index < kCount; ++index) {
      titles_[index] = label(8, index * 38, 18, &lv_font_noto_sans_sc_14);
      lv_obj_set_width(titles_[index], 120);
      statuses_[index] = label(132, index * 38, 18, &lv_font_noto_sans_sc_14);
      lv_obj_set_width(statuses_[index], 100);
      lv_obj_set_style_text_align(statuses_[index], LV_TEXT_ALIGN_RIGHT, 0);
      data_[index] = label(8, index * 38 + 18, 18, &lv_font_jetbrains_mono_14);
      lv_obj_set_style_text_color(statuses_[index], lv_color_make(100, 116, 139), 0);
      lv_label_set_long_mode(data_[index], LV_LABEL_LONG_DOT);
      lv_label_set_text(data_[index], "--");
    }
  }
  void status(unsigned index, const char *name, const char *value) {
    if (index >= kCount || !list_) return;
    set(titles_[index], name);
    set(statuses_[index], value);
  }
  void data(unsigned index, const char *value) {
    if (index < kCount && list_) set(data_[index], value);
  }
  lv_obj_t *list() const { return list_; }
  lv_obj_t *version() const { return version_; }
  lv_obj_t *reading(unsigned index) const { return data_[index]; }
  lv_obj_t *title(unsigned index) const { return titles_[index]; }
private:
  static void set(lv_obj_t *label, const char *value) {
    if (strcmp(lv_label_get_text(label), value)) lv_label_set_text(label, value);
  }
  lv_obj_t *label(int x, int y, int height, const lv_font_t *font) {
    lv_obj_t *result = lv_label_create(list_);
    lv_obj_set_pos(result, x, y);
    lv_obj_set_size(result, 216, height);
    lv_label_set_long_mode(result, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(result, font, 0);
    lv_obj_set_style_text_color(result, lv_color_make(30, 41, 59), 0);
    return result;
  }
  lv_obj_t *list_ = nullptr;
  lv_obj_t *version_ = nullptr;
  lv_obj_t *titles_[kCount] = {}, *statuses_[kCount] = {}, *data_[kCount] = {};
};

class PostureSetupView {
public:
  void create(lv_obj_t *parent, lv_event_cb_t placement, lv_event_cb_t calibration) {
    result_ = lv_label_create(parent);
    lv_obj_set_pos(result_, 12, 78);
    lv_obj_set_size(result_, 216, 58);
    lv_label_set_long_mode(result_, LV_LABEL_LONG_WRAP);
    title_ = lv_label_create(parent);
    lv_obj_set_pos(title_, 12, 142);
    lv_obj_set_size(title_, 216, 18);
    dropdown_ = lv_dropdown_create(parent);
    lv_obj_set_pos(dropdown_, 12, 164);
    lv_obj_set_size(dropdown_, 216, 36);
    lv_obj_set_style_radius(dropdown_, 4, 0);
    lv_obj_add_event_cb(dropdown_, placement, LV_EVENT_VALUE_CHANGED, nullptr);
    for (unsigned index = 0; index < 2; ++index) {
      buttons_[index] = lv_btn_create(parent);
      lv_obj_set_pos(buttons_[index], 12 + index * 112, 208);
      lv_obj_set_size(buttons_[index], 104, 36);
      lv_obj_set_style_radius(buttons_[index], 4, 0);
      lv_obj_set_style_bg_color(buttons_[index], lv_color_make(226, 232, 240), 0);
      lv_obj_add_event_cb(buttons_[index], calibration, LV_EVENT_CLICKED, (void *)(uintptr_t)index);
      buttonLabels_[index] = lv_label_create(buttons_[index]);
      lv_obj_center(buttonLabels_[index]);
      lv_obj_set_style_text_color(buttonLabels_[index], lv_color_make(30, 41, 59), 0);
    }
    language(false);
    select(RiderPosture::Placement::Forearm);
  }

  void language(bool chinese) {
    if (!dropdown_) return;
    chinese_ = chinese;
    const uint16_t selected = lv_dropdown_get_selected(dropdown_);
    lv_dropdown_set_options(dropdown_, chinese ? "右前臂（推荐）\n上臂\n背部心率带\n头盔固定" :
                                               "Right forearm (recommended)\nUpper arm\nBack HR strap\nHelmet mount");
    lv_dropdown_set_selected(dropdown_, selected);
    lv_label_set_text(title_, chinese ? "佩戴方式" : "WEAR LOCATION");
    lv_label_set_text(buttonLabels_[0], chinese ? LV_SYMBOL_REFRESH " 直立标定" : LV_SYMBOL_REFRESH " UPRIGHT");
    lv_label_set_text(buttonLabels_[1], chinese ? LV_SYMBOL_REFRESH " 前倾标定" : LV_SYMBOL_REFRESH " LEAN");
    for (auto *label : {title_, result_, buttonLabels_[0], buttonLabels_[1]})
      lv_obj_set_style_text_font(label, &lv_font_noto_sans_sc_14, 0);
    lv_obj_set_style_text_color(result_, lv_color_make(15, 118, 110), 0);
    lv_obj_set_style_text_font(dropdown_, &lv_font_noto_sans_sc_14, 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(dropdown_), &lv_font_noto_sans_sc_14, 0);
    update(estimate_);
  }

  void select(RiderPosture::Placement placement) {
    if (dropdown_) lv_dropdown_set_selected(dropdown_, optionFor(placement));
  }
  RiderPosture::Placement placement() const {
    switch (lv_dropdown_get_selected(dropdown_)) {
      case 0: return RiderPosture::Placement::Forearm;
      case 1: return RiderPosture::Placement::UpperArm;
      case 2: return RiderPosture::Placement::BackStrap;
      default: return RiderPosture::Placement::Helmet;
    }
  }
  void update(const RiderPosture::Estimate &estimate) {
    estimate_ = estimate;
    if (!result_) return;
    const char *state = "";
    using State = RiderPosture::State;
    const bool ready = estimate.state != State::NoData && estimate.state != State::Moving;
    for (unsigned index = 0; index < 2; ++index) {
      if (ready && (!index || estimate.state != State::NeedUpright))
        lv_obj_clear_state(buttons_[index], LV_STATE_DISABLED);
      else lv_obj_add_state(buttons_[index], LV_STATE_DISABLED);
    }
    switch (estimate.state) {
      case State::NoData: state = chinese_ ? "等待双源数据" : "Waiting for both IMUs"; break;
      case State::Moving: state = chinese_ ? "运动中，暂不判断" : "Motion / uncertain"; break;
      case State::NeedUpright: state = chinese_ ? "未完成直立标定" : "Upright not calibrated"; break;
      case State::NeedLean: state = chinese_ ? "未完成前倾标定" : "Lean not calibrated"; break;
      case State::Settling: state = chinese_ ? "稳定判断中" : "Settling"; break;
      case State::Upright: state = chinese_ ? "直立" : "Upright"; break;
      case State::Forward: state = chinese_ ? "前倾" : "Forward lean"; break;
      case State::Low: state = chinese_ ? "低姿态" : "Low position"; break;
    }
    const bool calibrated = estimate.state == State::Settling || estimate.state == State::Upright ||
        estimate.state == State::Forward || estimate.state == State::Low;
    char text[160];
    if (calibrated)
      snprintf(text, sizeof(text), "%s: %s\n%+.1f deg / %s",
               chinese_ ? "预估" : "EST", state, estimate.relativeDegrees,
               estimate.indirect ? (chinese_ ? "间接估计" : "indirect") : (chinese_ ? "试验估计" : "provisional"));
    else
      snprintf(text, sizeof(text), "%s\n%s", chinese_ ? "骑姿预估" : "POSTURE ESTIMATE", state);
    if (strcmp(lv_label_get_text(result_), text)) lv_label_set_text(result_, text);
  }
  lv_obj_t *dropdown() const { return dropdown_; }
  lv_obj_t *result() const { return result_; }
  lv_obj_t *button(unsigned index) const { return buttons_[index]; }
private:
  static uint16_t optionFor(RiderPosture::Placement placement) {
    switch (placement) {
      case RiderPosture::Placement::Forearm: return 0;
      case RiderPosture::Placement::UpperArm: return 1;
      case RiderPosture::Placement::BackStrap: return 2;
      case RiderPosture::Placement::Helmet: return 3;
    }
    return 0;
  }
  lv_obj_t *title_ = nullptr, *dropdown_ = nullptr, *result_ = nullptr;
  lv_obj_t *buttons_[2] = {}, *buttonLabels_[2] = {};
  RiderPosture::Estimate estimate_;
  bool chinese_ = false;
};