#pragma once

#include "ui_navigation_guard.h"
#include <lvgl.h>

class UiTouchInput {
public:
  using Sample = UiNavigationGuard::Sample;
  // Custom drag controls still need RELEASED, but never a synthetic click.
  static constexpr lv_obj_flag_t DragRelease = LV_OBJ_FLAG_USER_1;

  void attach(lv_indev_t *input, lv_obj_t *screen) {
    input_ = input;
    screen_ = screen;
    observe(screen);
  }

  void read(Sample state, int16_t x, int16_t y, uint32_t now, lv_indev_data_t *data) {
    if (session_.sample(state,x,y,now)) lv_indev_wait_release(input_);
    if (session_.started()) {
      scrolled_ = false;
      lv_point_t point = {x,y};
      auto *display = input_->driver->disp;
      auto *object = lv_indev_search_obj(lv_disp_get_layer_sys(display),&point);
      if (!object) object = lv_indev_search_obj(lv_disp_get_layer_top(display),&point);
      if (!object) object = lv_indev_search_obj(lv_disp_get_scr_act(display),&point);
      for (;object;object=lv_obj_get_parent(object))
        if (lv_obj_has_state(object,LV_STATE_SCROLLED)) scrolled_ = true;
    }
    data->point.x = session_.x();
    data->point.y = session_.y();
    data->state = session_.pressed() ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
  }

  static void pageChanged(lv_obj_t *object) {
    lv_event_send(lv_obj_get_screen(object),navigationEvent(),nullptr);
  }
  static void setPageHidden(lv_obj_t *object, bool hidden) {
    if (lv_obj_has_flag(object,LV_OBJ_FLAG_HIDDEN)==hidden) return;
    if (lv_obj_is_visible(lv_obj_get_parent(object))) pageChanged(object);
    if (hidden) lv_obj_add_flag(object,LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(object,LV_OBJ_FLAG_HIDDEN);
  }

private:
  static lv_event_code_t navigationEvent() {
    static const auto code = static_cast<lv_event_code_t>(lv_event_register_id());
    return code;
  }
  void observe(lv_obj_t *object) {
    if (lv_obj_get_event_user_data(object,event)!=this)
      lv_obj_add_event_cb(object,event,static_cast<lv_event_code_t>(LV_EVENT_ALL|LV_EVENT_PREPROCESS),this);
    for (unsigned i=0;i<lv_obj_get_child_cnt(object);++i)
      observe(lv_obj_get_child(object,i));
  }
  static void event(lv_event_t *event) {
    auto &self = *static_cast<UiTouchInput *>(lv_event_get_user_data(event));
    const auto code = lv_event_get_code(event);
    auto *object = lv_event_get_current_target(event);
    if (object==self.screen_) {
      if (code==LV_EVENT_CHILD_CREATED) {
        // Also covers controls created later, including dropdown popups.
        auto *child = static_cast<lv_obj_t *>(lv_event_get_param(event));
        self.observe(child ? child : lv_event_get_target(event));
      } else if (code==navigationEvent()) {
        if (self.session_.navigate()) lv_indev_wait_release(self.input_);
      }
    }
    if (object!=lv_event_get_target(event) || lv_indev_get_act()!=self.input_) return;
    if (code==LV_EVENT_SCROLL_BEGIN && self.session_.pressed()) self.scrolled_=true;
    if (self.session_.clickAllowed() && !self.scrolled_) return;
    if (code==LV_EVENT_CLICKED || code==LV_EVENT_SHORT_CLICKED ||
        code==LV_EVENT_LONG_PRESSED || code==LV_EVENT_LONG_PRESSED_REPEAT) {
      lv_event_stop_processing(event);
    } else if (code==LV_EVENT_RELEASED && !lv_obj_has_flag(object,DragRelease) &&
               !lv_obj_check_type(object,&lv_slider_class)) {
      // Run before the widget class can toggle a checkbox or select a dropdown row.
      lv_event_send(object,LV_EVENT_PRESS_LOST,self.input_);
      lv_event_stop_processing(event);
    }
  }
  lv_indev_t *input_ = nullptr;
  lv_obj_t *screen_ = nullptr;
  UiNavigationGuard session_;
  bool scrolled_ = false;
};
