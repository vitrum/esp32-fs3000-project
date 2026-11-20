#include "display_lcd.h"
#include "../../src/audio.h"
#include <cassert>
#include <iostream>
#include <string>
#include <fstream>

std::atomic<uint32_t> clockOffset{0};
HostSerial Serial;
bool failTaskCreation = false;
TwoWire Wire1;
SdActivity sdActivity;
bool sdCardReady() { return false; }

static UiTouchInput touch;
static bool pressed = false;
static lv_point_t point{};

static void readPointer(lv_indev_drv_t *, lv_indev_data_t *data) {
  touch.read(pressed ? UiTouchInput::Sample::Pressed : UiTouchInput::Sample::Released,
             point.x, point.y, lv_tick_get(), data);
}

static void ticks(unsigned count = 8) {
  for (unsigned i = 0; i < count; ++i) {
    clockOffset += 10;
    lv_tick_inc(10);
    lv_timer_handler();
  }
}

static void tap(lv_coord_t x, lv_coord_t y) {
  point = {x, y};
  pressed = true;
  ticks();
  pressed = false;
  ticks();
}

static lv_obj_t *findText(lv_obj_t *object, const char *text) {
  if (lv_obj_check_type(object, &lv_label_class) &&
      std::string(lv_label_get_text(object)) == text) return object;
  for (uint32_t i = 0; i < lv_obj_get_child_cnt(object); ++i)
    if (auto *found = findText(lv_obj_get_child(object, i), text)) return found;
  return nullptr;
}

static lv_obj_t *findType(lv_obj_t *object, const lv_obj_class_t *type) {
  if (lv_obj_check_type(object, type)) return object;
  for (uint32_t i = 0; i < lv_obj_get_child_cnt(object); ++i)
    if (auto *found = findType(lv_obj_get_child(object, i), type)) return found;
  return nullptr;
}

static void clickLabel(lv_obj_t *root, const char *text) {
  auto *label = findText(root, text);
  assert(label);
  auto *button = lv_obj_get_parent(label);
  lv_obj_update_layout(root);
  lv_area_t area;
  lv_obj_get_coords(button, &area);
  lv_point_t center{lv_coord_t((area.x1 + area.x2) / 2), lv_coord_t((area.y1 + area.y2) / 2)};
  assert(lv_indev_search_obj(lv_scr_act(), &center) == button);
  tap(center.x, center.y);
}

static void selectTab(lv_obj_t *tabs, unsigned index) {
  lv_obj_update_layout(tabs);
  lv_area_t area;
  lv_obj_get_coords(lv_tabview_get_tab_btns(tabs), &area);
  tap(area.x1 + 24 + index * 48, (area.y1 + area.y2) / 2);
  ticks(30);
  assert(lv_tabview_get_tab_act(tabs) == index);
  auto *tab = lv_obj_get_child(lv_tabview_get_content(tabs), index);
  for (uint32_t i = 0; i < lv_obj_get_child_cnt(tab); ++i) {
    lv_area_t child;
    lv_obj_get_coords(lv_obj_get_child(tab, i), &child);
    assert(child.x1 >= 0 && child.x2 < 240 && child.y1 >= 84 && child.y2 < 320);
  }
}

static void dragVolume(DisplayLcd &display, lv_obj_t *slider, bool maximum) {
  lv_obj_update_layout(slider);
  lv_area_t area;
  lv_obj_get_coords(slider, &area);
  point = {lv_coord_t(area.x1 + (area.x2 - area.x1) * lv_slider_get_value(slider) / 100),
           lv_coord_t((area.y1 + area.y2) / 2)};
  pressed = true;
  ticks();
  point.x = maximum ? area.x2 : area.x1;
  ticks();
  uint8_t volume;
  assert(!display.takeVolumeRequest(volume));
  pressed = false;
  ticks();
  assert(display.takeVolumeRequest(volume) && volume == (maximum ? 100 : 0));
  assert(!display.takeVolumeRequest(volume));
}

static void saveFrame(const char *name) {
  const char *directory = std::getenv("UI_ARTIFACT_DIR");
  if (!directory) return;
  auto *display = lv_disp_get_default();
  lv_obj_invalidate(lv_scr_act());
  lv_refr_now(display);
  std::ofstream out(std::string(directory) + "/" + name, std::ios::binary);
  out << "P6\n240 320\n255\n";
  for (const uint16_t color : hostFramebuffer) {
    const char rgb[] = {char(((color >> 11) & 31) * 255 / 31),
                        char(((color >> 5) & 63) * 255 / 63), char((color & 31) * 255 / 31)};
    out.write(rgb, sizeof(rgb));
  }
  assert(out.good());
}

int main() {
  Preferences preferences;
  assert(preferences.begin("aero-ui", false));
  assert(preferences.putBool("zh", true) == 1);
  assert(preferences.putUChar("timezone", uint8_t(SystemSettings::Timezone::UTC_N5)) == 1);
  Wire1.ack = false;
  DisplayLcd display;
  display.begin();
  auto *screen = lv_scr_act();
  static lv_indev_drv_t inputDriver;
  lv_indev_drv_init(&inputDriver);
  inputDriver.type = LV_INDEV_TYPE_POINTER;
  inputDriver.read_cb = readPointer;
  auto *input = lv_indev_drv_register(&inputDriver);
  touch.attach(input, screen);
  display.showSettings();
  clickLabel(screen, LV_SYMBOL_SETTINGS "\n系统设置");
  auto *title = findText(screen, "系统设置");
  assert(title);
  auto *panel = lv_obj_get_parent(title);
  assert(lv_obj_is_visible(panel));
  auto *tabs = findType(panel, &lv_tabview_class);
  assert(tabs);
  auto *content = lv_tabview_get_content(tabs);
  auto *time = lv_obj_get_child(content, 0);
  auto *sound = lv_obj_get_child(content, 1);
  auto *language = lv_obj_get_child(content, 2);
  auto *wifi = lv_obj_get_child(content, 3);
  auto *usb = lv_obj_get_child(content, 4);
  display.updateClock(1767225600);
  assert(findText(screen, "19:00:00"));
  clickLabel(time, "UTC+8");
  assert(findText(screen, "08:00:00"));
  selectTab(tabs, 2);
  clickLabel(language, "English");

  bool enabled = false;
  selectTab(tabs, 3);
  clickLabel(wifi, "ON");
  assert(display.takeWifiEnableRequest(enabled) && enabled);
  assert(!display.takeWifiEnableRequest(enabled));
  display.updateWifiStatus(true, true, false, "test-network", "192.0.2.5");
  assert(findText(wifi, "Connected: test-network\nhttp://aeroprobe.local/\nhttp://192.0.2.5/"));
  saveFrame("system-wifi-en.ppm");
  clickLabel(wifi, "OFF");
  assert(display.takeWifiEnableRequest(enabled) && !enabled);
  display.updateWifiStatus(false, false, false, "", "");
  assert(findText(wifi, "Wi-Fi is off"));
  clickLabel(wifi, "SET UP WI-FI");
  assert(display.takeWifiSetupRequest() && !display.takeWifiSetupRequest());

  selectTab(tabs, 1);
  display.setVolume(37);
  auto *slider = findType(sound, &lv_slider_class);
  assert(slider && lv_slider_get_value(slider) == 37 && findText(sound, "37%"));
  uint8_t volume = 0;
  lv_slider_set_value(slider, 82, LV_ANIM_OFF);
  lv_event_send(slider, LV_EVENT_VALUE_CHANGED, nullptr);
  assert(findText(sound, "82%") && !display.takeVolumeRequest(volume));
  lv_event_send(slider, LV_EVENT_RELEASED, nullptr);
  assert(display.takeVolumeRequest(volume) && volume == 82);
  assert(!display.takeVolumeRequest(volume));
  audio.setVolume(volume);
  assert(audio.volume() == 82);
  dragVolume(display, slider, false);
  assert(findText(sound, "0%"));
  dragVolume(display, slider, true);
  assert(findText(sound, "100%"));
  saveFrame("system-sound-en.ppm");

  selectTab(tabs, 2);
  clickLabel(language, "简体中文");
  assert(preferences.begin("aero-ui", true));
  assert(preferences.getBool("zh", false));
  assert(findText(panel, "系统设置") && findText(wifi, "配置网络"));
  assert(findText(sound, "音量:") && findText(wifi, "Wi-Fi 已关闭"));
  saveFrame("system-language-zh.ppm");
  selectTab(tabs, 3);
  clickLabel(wifi, "开启");
  assert(display.takeWifiEnableRequest(enabled) && enabled);
  selectTab(tabs, 2);
  clickLabel(language, "English");
  assert(!preferences.getBool("zh", true));
  assert(findText(panel, "SYSTEM SETTINGS") && findText(wifi, "SET UP WI-FI"));

  selectTab(tabs, 0);
  assert(!findText(time, "Time Sync: OK"));
  display.updateNetworkTime(false, 0);
  assert(findText(time, "Wi-Fi is disconnected") && findText(time, "UTC  --"));
  display.updateNetworkTime(true, 1767225600);
  assert(findText(time, "NTP time synchronized"));
  saveFrame("system-time-en.ppm");
  display.updateClock(1767225600);
  assert(findText(screen, "08:00:00"));
  clickLabel(time, "UTC-5");
  assert(findText(screen, "19:00:00"));
  assert(preferences.getUChar("timezone", 255) == uint8_t(SystemSettings::Timezone::UTC_N5));
  Preferences::failWrite = true;
  clickLabel(time, "UTC+8");
  assert(findText(screen, "Could not save timezone."));
  assert(findText(screen, "19:00:00"));
  Preferences::failWrite = false;
  assert(preferences.getUChar("timezone", 255) == uint8_t(SystemSettings::Timezone::UTC_N5));
  clickLabel(screen, "OK");

  selectTab(tabs, 4);
  clickLabel(usb, "ON");
  assert(display.takeUsbStorageRequest(enabled) && enabled);
  assert(!display.takeUsbStorageRequest(enabled));
  saveFrame("system-usb-en.ppm");
  display.showUsbStorageError("Stop recording before USB.");
  assert(findText(usb, "Stop recording before USB."));
  auto *notice = lv_obj_get_child(screen, -1);
  assert(lv_obj_is_visible(notice) && findText(notice, "Stop recording before USB."));
  assert(lv_mem_test() == LV_RES_OK);
  std::cout << "PASS system settings: Wi-Fi commands/status, volume release, language persistence, time, USB errors\n";
}
