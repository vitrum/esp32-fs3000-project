#pragma once

#include <lvgl.h>
#include <cstring>
#include <cstdio>
#include <initializer_list>
#include "jetbrains_mono_latin.h"
#include "rider_posture.h"
#include "imu_mount_calibration.h"

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
  void create(lv_obj_t *parent, lv_event_cb_t placement, lv_event_cb_t calibration,
              lv_coord_t top = 78) {
    result_ = lv_label_create(parent);
    lv_obj_set_pos(result_, 12, top);
    lv_obj_set_size(result_, 216, 58);
    lv_label_set_long_mode(result_, LV_LABEL_LONG_WRAP);
    title_ = lv_label_create(parent);
    lv_obj_set_pos(title_, 12, top + 64);
    lv_obj_set_size(title_, 216, 18);
    dropdown_ = lv_dropdown_create(parent);
    lv_obj_set_pos(dropdown_, 12, top + 86);
    lv_obj_set_size(dropdown_, 216, 36);
    lv_obj_set_style_radius(dropdown_, 4, 0);
    lv_obj_add_event_cb(dropdown_, placement, LV_EVENT_VALUE_CHANGED, nullptr);
    for (unsigned index = 0; index < 2; ++index) {
      buttons_[index] = lv_btn_create(parent);
      lv_obj_set_pos(buttons_[index], 12 + index * 112, top + 130);
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

class ImuCalibrationView {
public:
  enum Page : unsigned { Connection, Wearing, Axis, Calibration, Posture, PageCount };
  void create(lv_obj_t *parent, lv_event_cb_t action) {
    root_=lv_obj_create(parent);
    lv_obj_set_pos(root_,0,52); lv_obj_set_size(root_,240,268);
    plain(root_);
    for (auto &page : pages_) {
      page=lv_obj_create(root_);
      lv_obj_set_pos(page,0,0); lv_obj_set_size(page,240,216);
      plain(page);
    }
    connectionStatus_=label(Connection,8,0,224,70);
    lv_label_set_text(connectionStatus_,"--");
    connect_=lv_btn_create(pages_[Connection]);
    lv_obj_set_pos(connect_,16,82); lv_obj_set_size(connect_,208,42);
    lv_obj_set_style_bg_color(connect_,lv_color_make(37,99,235),0);
    connectLabel_=lv_label_create(connect_); lv_obj_center(connectLabel_);
    title_=label(Wearing,8,0,224,22);
    const lv_point_t points[]={{15,74},{150,74},{137,65},{150,74},{137,83}};
    for (unsigned i=0;i<5;++i) arrowPoints_[i]=points[i];
    auto *arrow=lv_line_create(pages_[Wearing]);
    lv_obj_clear_flag(arrow,LV_OBJ_FLAG_SCROLLABLE);
    lv_line_set_points(arrow,arrowPoints_,5);
    lv_obj_set_style_line_width(arrow,3,0);
    auto *sensor=lv_obj_create(pages_[Wearing]);
    lv_obj_set_pos(sensor,100,32); lv_obj_set_size(sensor,66,34);
    lv_obj_clear_flag(sensor,LV_OBJ_FLAG_SCROLLABLE);
    auto *logo=lv_label_create(sensor); lv_label_set_text(logo,"LOGO"); lv_obj_center(logo);
    auto *usb=label(Wearing,12,35,85,22); lv_label_set_text(usb,"Type-C <");
    direction_=label(Wearing,8,92,224,22);
    wearing_=label(Wearing,8,120,224,90);
    instruction_=label(Calibration,8,0,224,112);
    status_=label(Calibration,8,116,224,54);
    axisLabel_=label(Axis,8,8,224,40);
    axis_=lv_dropdown_create(pages_[Axis]); lv_obj_set_pos(axis_,8,56); lv_obj_set_size(axis_,224,36);
    lv_dropdown_set_options(axis_,"+X\n+Y\n+Z\n-X\n-Y\n-Z");
    checked_=lv_checkbox_create(pages_[Axis]); lv_obj_set_pos(checked_,8,104);
    lv_obj_set_width(checked_,224);
    axisHint_=label(Axis,8,136,224,32);
    for (unsigned i=0;i<3;++i) {
      buttons_[i]=lv_btn_create(pages_[i ? Calibration : Axis]);
      lv_obj_set_pos(buttons_[i],i==2 ? 108 : 8,i ? 178 : 174);
      lv_obj_set_size(buttons_[i],!i ? 224 : i==1 ? 92 : 124,38);
      lv_obj_add_event_cb(buttons_[i],action,LV_EVENT_CLICKED,(void *)(uintptr_t)i);
      buttonLabels_[i]=lv_label_create(buttons_[i]); lv_obj_center(buttonLabels_[i]);
    }
    for (unsigned i=0;i<2;++i) {
      navigation_[i]=lv_btn_create(root_);
      lv_obj_set_pos(navigation_[i],i ? 160 : 8,224);
      lv_obj_set_size(navigation_[i],72,38);
      lv_obj_add_event_cb(navigation_[i],navigate,LV_EVENT_CLICKED,this);
      navigationLabels_[i]=lv_label_create(navigation_[i]); lv_obj_center(navigationLabels_[i]);
    }
    pageLabel_=lv_label_create(root_);
    lv_obj_set_pos(pageLabel_,84,234); lv_obj_set_size(pageLabel_,72,20);
    lv_obj_set_style_text_align(pageLabel_,LV_TEXT_ALIGN_CENTER,0);
    language(false);
    showPage(Connection);
  }
  void language(bool chinese) {
    chinese_=chinese;
    if (!root_) return;
    lv_label_set_text(title_,chinese?"前臂轴向确认":"FOREARM ALIGNMENT");
    lv_label_set_text(direction_,chinese?"手肘 <           > 车头":"ELBOW <       > FRONT");
    lv_label_set_text(wearing_,chinese?
      "Type-C 朝手肘。\nLOGO、按钮面与手背同侧。\n停车，扶稳车身，前轮摆正。":
      "USB toward elbow.\nLOGO/buttons face hand back.\nStop; support bike.\nKeep front wheel straight.");
    lv_label_set_text(axisLabel_,chinese?"主板哪个原生轴指向车头？":"Board native axis toward front?");
    lv_label_set_text(axisHint_,chinese?"不知道主板轴向时不要猜。":"Unknown axis? Do not guess.");
    lv_checkbox_set_text(checked_,chinese?"已核对轴向与佩戴":"Axes/wearing checked");
    lv_label_set_text(connectLabel_,chinese?"连接 / 搜索设备":"CONNECT / SEARCH");
    lv_label_set_text(navigationLabels_[0],chinese?"上一页":"PREV");
    lv_label_set_text(navigationLabels_[1],chinese?"下一页":"NEXT");
    for (auto *object : {title_,direction_,wearing_,instruction_,status_,axisLabel_,axisHint_,
                         checked_,axis_,connectionStatus_,connectLabel_,navigationLabels_[0],
                         navigationLabels_[1],pageLabel_})
      lv_obj_set_style_text_font(object,&lv_font_noto_sans_sc_14,0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(axis_),&lv_font_noto_sans_sc_14,0);
    const char *zh[]={"确认并开始","应用标定","取消/原始"};
    const char *en[]={"START","APPLY","CANCEL/RAW"};
    for (unsigned i=0;i<3;++i) {
      lv_label_set_text(buttonLabels_[i],chinese?zh[i]:en[i]);
      lv_obj_set_style_text_font(buttonLabels_[i],&lv_font_noto_sans_sc_14,0);
    }
    update(stage_,error_,cycles_,blocked_);
  }
  void update(ImuMountCalibration::Stage stage, ImuMountCalibration::Error error,
              unsigned cycles, bool blocked) {
    const bool changed=stage_!=stage;
    stage_=stage; error_=error; cycles_=cycles; blocked_=blocked;
    if (!root_) return;
    using Stage=ImuMountCalibration::Stage;
    const char *zh[]={
      "蓝牙未连接。\n连接后等待双 IMU 原始数据。",
      "蓝牙已连接。\n等待双 IMU 新鲜原始数据。",
      "先确认佩戴与主板车头轴。\n静止、翻掌、抬臂、握把验证。\n不知道主板轴向时不要猜。",
      "手指朝车头，前臂水平。\n手心向下，LOGO 朝上。\n保持静止 3 秒。",
      "前臂仍指向车头。\n先翻到拇指朝上，再回掌朝下。\n缓慢往返两次，不只弯手腕。",
      "前臂从水平抬起 30-45 度。\n再放回水平，重复两次。\n车身与码表保持静止。",
      "正常握住右手变头。\n保持 3 秒，腕带不要滑动。\n软件不能证明手掌接触。",
      "数值检查通过，待人工确认。\n确认握住右手变头且未滑动。\n点击应用；不代表实测定位。",
      "本次轴向标定已应用。\n原始数据保留，另记参考向量。\n重连或换佩戴后需重做。",
      "本次标定无效。\n检查原因，停车后重新开始。\n也可取消，仅记录原始数据。"};
    const char *en[]={
      "BLE offline.\nConnect and wait for raw IMUs.",
      "BLE linked.\nWaiting for fresh raw IMUs.",
      "Check wearing and board axis.\nStill, roll, lift, hood check.\nDo not guess board forward axis.",
      "Forearm level, fingers forward.\nPalm down, LOGO up.\nHold still for 3 seconds.",
      "Keep forearm pointing forward.\nThumb up, then palm down.\nRepeat twice; rotate forearm.",
      "Lift forearm 30-45 degrees.\nReturn level; repeat twice.\nKeep bike/computer still.",
      "Grip the right hood normally.\nHold 3 seconds; no strap slip.\nContact is not auto-detected.",
      "Checks passed; confirm contact\nand unchanged strap placement.\nAPPLY is not position tracking.",
      "Applied for this connection.\nRaw + fixed reference vectors.\nReconnect/remount: repeat.",
      "Calibration invalid.\nCheck reason, stop and retry.\nOr cancel and record raw only."};
    set(instruction_,chinese_?zh[unsigned(stage)]:en[unsigned(stage)]);
    const char *reasonsZh[]={"正常","数据中断 / 不完整","码表移动，请扶稳","动作方向或幅度不符",
                            "转轴不符 / 动作不纯","超时，请重试","有效采样不足 20 Hz","已取消"};
    const char *reasonsEn[]={"OK","Missing/stale raw data","Board moved","Wrong motion/range",
                            "Wrong/impure axis","Timed out","Samples below 20 Hz","Cancelled"};
    char text[160];
    snprintf(text,sizeof(text),"%s\n%s %u/2%s",chinese_?reasonsZh[unsigned(error)]:reasonsEn[unsigned(error)],
      chinese_?"动作进度":"Cycles",cycles,blocked?(chinese_?"\n记录/存储忙，请先停止":"\nRecording/storage busy"):"");
    set(status_,text);
    enabled(buttons_[0],!blocked && (stage==Stage::Wearing || stage==Stage::Failed || stage==Stage::Ready));
    enabled(buttons_[1],!blocked && stage==Stage::Confirm);
    enabled(axis_,stage==Stage::Wearing || stage==Stage::Failed || stage==Stage::Ready);
    enabled(checked_,stage==Stage::Wearing || stage==Stage::Failed || stage==Stage::Ready);
    if (changed && stage>=Stage::Static && stage<=Stage::Failed) showPage(Calibration);
  }
  void quality(const ImuMountCalibration::Result &result) {
    if (!status_ || blocked_ || (stage_!=ImuMountCalibration::Stage::Confirm &&
                                 stage_!=ImuMountCalibration::Stage::Ready)) return;
    char text[120];
    snprintf(text,sizeof(text),"%s %.0f%% / %.0f%%\n%s %lu / %lu",
      chinese_?"转轴误差":"Off-axis",result.rollOffAxis*100,result.liftOffAxis*100,
      chinese_?"有效样本":"Samples",(unsigned long)result.rollSamples,(unsigned long)result.liftSamples);
    set(status_,text);
  }
  bool checked() const { return lv_obj_has_state(checked_,LV_STATE_CHECKED); }
  void resetCheck() {
    if (!checked_) return;
    lv_obj_clear_state(checked_,LV_STATE_CHECKED);
    showPage(Connection);
  }
  int8_t forwardAxis() const {
    const unsigned selected=lv_dropdown_get_selected(axis_);
    return selected<3 ? int8_t(selected+1) : -int8_t(selected-2);
  }
  lv_obj_t *button(unsigned i) const { return buttons_[i]; }
  lv_obj_t *instruction() const { return instruction_; }
  lv_obj_t *status() const { return status_; }
  lv_obj_t *root() const { return root_; }
  lv_obj_t *page(Page page) const { return pages_[page]; }
  Page currentPage() const { return page_; }
  lv_obj_t *navigation(unsigned i) const { return navigation_[i]; }
  lv_obj_t *connectButton() const { return connect_; }
  lv_obj_t *connectionStatus() const { return connectionStatus_; }
  lv_obj_t *axisDropdown() const { return axis_; }
  lv_obj_t *wearingCheck() const { return checked_; }
  void showPage(Page page) {
    page_=page;
    for (unsigned i=0;i<PageCount;++i) {
      if (i==page_) lv_obj_clear_flag(pages_[i],LV_OBJ_FLAG_HIDDEN);
      else lv_obj_add_flag(pages_[i],LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text_fmt(pageLabel_,"%u / %u",unsigned(page_)+1,unsigned(PageCount));
    enabled(navigation_[0],page_!=Connection);
    enabled(navigation_[1],unsigned(page_)+1<PageCount);
  }
private:
  static void plain(lv_obj_t *object) {
    lv_obj_set_style_pad_all(object,0,0);
    lv_obj_set_style_border_width(object,0,0);
    lv_obj_set_style_radius(object,0,0);
    lv_obj_set_style_bg_opa(object,LV_OPA_TRANSP,0);
    lv_obj_clear_flag(object,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(object,LV_SCROLLBAR_MODE_OFF);
  }
  static void navigate(lv_event_t *event) {
    auto *self=static_cast<ImuCalibrationView *>(lv_event_get_user_data(event));
    if (lv_event_get_target(event)==self->navigation_[0]) {
      if (self->page_!=Connection) self->showPage(Page(unsigned(self->page_)-1));
    } else if (unsigned(self->page_)+1<PageCount) self->showPage(Page(unsigned(self->page_)+1));
  }
  static void set(lv_obj_t *object,const char *text) {
    if (strcmp(lv_label_get_text(object),text)) lv_label_set_text(object,text);
  }
  static void enabled(lv_obj_t *object,bool enabled) {
    if (enabled) lv_obj_clear_state(object,LV_STATE_DISABLED); else lv_obj_add_state(object,LV_STATE_DISABLED);
  }
  lv_obj_t *label(Page page,int x,int y,int width,int height) {
    auto *object=lv_label_create(pages_[page]); lv_obj_set_pos(object,x,y); lv_obj_set_size(object,width,height);
    lv_label_set_long_mode(object,LV_LABEL_LONG_WRAP); return object;
  }
  lv_obj_t *root_=nullptr,*title_=nullptr,*direction_=nullptr,*wearing_=nullptr,*instruction_=nullptr,*status_=nullptr;
  lv_obj_t *axisLabel_=nullptr,*axis_=nullptr,*checked_=nullptr,*buttons_[3]={},*buttonLabels_[3]={};
  lv_obj_t *pages_[PageCount]={},*navigation_[2]={},*navigationLabels_[2]={},*pageLabel_=nullptr;
  lv_obj_t *connectionStatus_=nullptr,*connect_=nullptr,*connectLabel_=nullptr,*axisHint_=nullptr;
  Page page_=Connection;
  lv_point_t arrowPoints_[5]={};
  ImuMountCalibration::Stage stage_=ImuMountCalibration::Stage::Offline;
  ImuMountCalibration::Error error_=ImuMountCalibration::Error::None;
  unsigned cycles_=0;
  bool chinese_=false,blocked_=false;
};