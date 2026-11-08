#include "aero_zone_view.h"
#include "battery_gauge.h"
#include "dashboard_layout.h"
#include "dashboard_view.h"
#include "sensor_views.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <regex>
#include <set>
#include <vector>

static lv_color_t framebuffer[240 * 320];
static unsigned flushes = 0;
static bool pointerPressed = false;
static lv_point_t pointerPoint = {180, 60};

static void readPointer(lv_indev_drv_t *, lv_indev_data_t *data) {
  data->point = pointerPoint;
  data->state = pointerPressed ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
}

static void flush(lv_disp_drv_t *driver, const lv_area_t *area, lv_color_t *colors) {
  for (int y = area->y1; y <= area->y2; ++y)
    for (int x = area->x1; x <= area->x2; ++x) {
      assert(x >= 0 && x < 240 && y >= 0 && y < 320);
      framebuffer[y * 240 + x] = *colors++;
    }
  ++flushes;
  lv_disp_flush_ready(driver);
}

static bool overlaps(const lv_area_t &first, const lv_area_t &second) {
  return first.x1 <= second.x2 && first.x2 >= second.x1 &&
         first.y1 <= second.y2 && first.y2 >= second.y1;
}

static void saveFrame(const char *name) {
  const char *directory = std::getenv("UI_ARTIFACT_DIR");
  if (!directory) return;
  std::ofstream output(std::string(directory) + "/" + name, std::ios::binary);
  output << "P6\n216 88\n255\n";
  for (unsigned y = 43; y < 131; ++y) {
    for (unsigned x = 12; x < 228; ++x) {
      const uint16_t color = framebuffer[y * 240 + x].full;
      const char rgb[] = {char(((color >> 11) & 31) * 255 / 31),
                          char(((color >> 5) & 63) * 255 / 63), char((color & 31) * 255 / 31)};
      output.write(rgb, sizeof(rgb));
    }
  }
  assert(output.good());
}

static void checkView(const AeroZoneView &view) {
  lv_obj_update_layout(view.parent());
  const std::vector<lv_obj_t *> labels{view.title(), view.description(), view.zone(), view.unit(), view.value()};
  lv_area_t parent;
  lv_obj_get_coords(view.parent(), &parent);
  std::vector<lv_area_t> areas;
  for (auto *label : labels) {
    lv_area_t area;
    lv_obj_get_coords(label, &area);
    assert(area.x1 >= parent.x1 && area.x2 <= parent.x2 && area.y1 >= parent.y1 && area.y2 <= parent.y2);
    for (const auto &previous : areas) assert(!overlaps(area, previous));
    areas.push_back(area);
    lv_point_t textSize;
    lv_txt_get_size(&textSize, lv_label_get_text(label), lv_obj_get_style_text_font(label, LV_PART_MAIN),
                    lv_obj_get_style_text_letter_space(label, LV_PART_MAIN),
                    lv_obj_get_style_text_line_space(label, LV_PART_MAIN), LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    assert(textSize.x <= lv_obj_get_width(label) && textSize.y <= lv_obj_get_height(label));
  }
}

static void checkDeviceNavigation() {
  std::ifstream input(std::string(UI_SOURCE_ROOT) + "/src/display_lcd.cpp");
  assert(input.good());
  const std::string source{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  assert(source.find("buildPanelHeader(deviceHubPanel_, \"BLUETOOTH DEVICES\", onCancelSelection)") != std::string::npos);
  assert(source.find("buildPanelHeader(powerSourcePanel_, \"POWER SOURCE\", onDeviceRoleBack)") != std::string::npos);
  const size_t callbackStart = source.find("void DisplayLcd::onCancelSelection(");
  assert(callbackStart != std::string::npos);
  const size_t callbackEnd = source.find("\n}\n", callbackStart);
  assert(callbackEnd != std::string::npos);
  const std::string callback = source.substr(callbackStart, callbackEnd - callbackStart);
  assert(callback.find("hideDevicePanels()") != std::string::npos);
  assert(callback.find("lv_obj_clear_flag(g_display->settingsPanel_, LV_OBJ_FLAG_HIDDEN)") != std::string::npos);
  assert(callback.find("lv_obj_clear_flag(g_display->deviceHubPanel_") == std::string::npos);
}

static void checkGlyphs() {
  std::set<uint32_t> required;
  const std::regex literal(R"re("(?:\\.|[^"\\])*")re");
  for (const char *name : {"/src/display_lcd.cpp", "/src/aero_zone_view.h", "/src/dashboard_view.h", "/src/sensor_views.h"}) {
    std::ifstream input(std::string(UI_SOURCE_ROOT) + name);
    assert(input.good());
    const std::string source{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    for (std::sregex_iterator match(source.begin(), source.end(), literal), end; match != end; ++match) {
      const std::string text = match->str();
      uint32_t index = 0;
      while (index < text.size()) {
        const uint32_t codepoint = _lv_txt_encoded_next(text.c_str(), &index);
        if ((codepoint >= 0x4e00 && codepoint <= 0x9fff) ||
            (codepoint >= 0x3000 && codepoint <= 0x303f) || (codepoint >= 0xff00 && codepoint <= 0xffef))
          required.insert(codepoint);
      }
    }
  }
  assert(required.size() >= 150);
  bool allGlyphsPresent = true;
  for (const uint32_t codepoint : required) {
    lv_font_glyph_dsc_t glyph;
    if (!lv_font_get_glyph_dsc(&lv_font_noto_sans_sc_14, &glyph, codepoint, 0)) {
      std::fprintf(stderr, "Missing UI font glyph U+%04X\n", unsigned(codepoint));
      allGlyphsPresent = false;
      continue;
    }
    assert(lv_font_get_glyph_dsc(&lv_font_noto_sans_sc_14, &glyph, codepoint, 0));
    assert(!glyph.is_placeholder);
    if (glyph.box_w && glyph.box_h) assert(lv_font_get_glyph_bitmap(&lv_font_noto_sans_sc_14, codepoint));
  }
  assert(allGlyphsPresent);
  for (const char *symbol : {LV_SYMBOL_LEFT, LV_SYMBOL_SETTINGS, LV_SYMBOL_PLAY, LV_SYMBOL_SAVE}) {
    uint32_t index = 0;
    lv_font_glyph_dsc_t glyph;
    assert(lv_font_get_glyph_dsc(&lv_font_noto_sans_sc_14, &glyph, _lv_txt_encoded_next(symbol, &index), 0));
    assert(!glyph.is_placeholder);
  }
  std::cout << "CJK glyphs checked: " << required.size() << '\n';
}


// ---- 录制指示：红点/图标必须在整个录制期间持续闪烁，停止后恢复绿色 ----
static lv_color_t sdIconColor(DashboardView &view) {
  return lv_obj_get_style_text_color(view.sdIcon, 0);
}

static void checkRecordingIndicator(lv_disp_t *display) {
  const lv_color_t green = lv_color_make(34, 197, 94);
  const lv_color_t red = lv_color_make(239, 68, 68);
  lv_obj_t *screen = lv_disp_get_scr_act(display);
  lv_obj_clean(screen);
  DashboardView view;
  view.create(screen, [](lv_event_t *) {}, [](lv_event_t *) {});

  // 未录制：图标为绿色，不闪，红点隐藏。
  for (uint32_t now = 0; now <= 4000; now += 250) {
    view.storage(SdActivity::State::Ready, now, false);
    assert(sdIconColor(view).full == green.full);
    assert(lv_obj_has_flag(view.dot_, LV_OBJ_FLAG_HIDDEN));
  }

  // 录制中：跨越远超 600ms 的时间，颜色必须持续在两个值之间切换，
  // 也就是说红点不能「只闪一次」。
  unsigned redSeen = 0, greenSeen = 0, transitions = 0;
  uint16_t previous = 0xFFFF;
  for (uint32_t now = 0; now <= 20000; now += 250) {
    view.storage(SdActivity::State::Ready, now, true);
    const uint16_t color = sdIconColor(view).full;
    // 录制期间红点必须一直显示（不是只显示一次）。
    assert(!lv_obj_has_flag(view.dot_, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_get_style_bg_color(view.dot_, 0).full == red.full);
    assert(color == red.full || color == green.full);
    if (color == red.full) ++redSeen; else ++greenSeen;
    if (previous != 0xFFFF && color != previous) ++transitions;
    previous = color;
  }
  // 20s 内每 500ms 翻转一次 => 至少 35 次跳变；红/绿都必须出现很多次。
  assert(transitions >= 35);
  assert(redSeen >= 20 && greenSeen >= 20);

  // 停止录制后：立即恢复绿色，且不再闪烁，红点隐藏。
  for (uint32_t now = 20000; now <= 24000; now += 250) {
    view.storage(SdActivity::State::Ready, now, false);
    assert(sdIconColor(view).full == green.full);
    assert(lv_obj_has_flag(view.dot_, LV_OBJ_FLAG_HIDDEN));
  }

  // SD 错误优先级高于录制：仍为红色告警图标。
  view.storage(SdActivity::State::Error, 100, true);
  assert(sdIconColor(view).full == red.full);
  assert(std::string(lv_label_get_text(view.sdIcon)) == LV_SYMBOL_WARNING);

  // SD 拔出：灰色，不闪。
  view.storage(SdActivity::State::Absent, 100, true);
  assert(std::string(lv_label_get_text(view.sdIcon)) == LV_SYMBOL_SAVE);

  // 录制期间反复刷新不应泄漏或崩溃。
  for (unsigned repeat = 0; repeat < 200; ++repeat) {
    view.storage(SdActivity::State::Ready, repeat * 250, true);
    lv_refr_now(display);
  }
  assert(lv_mem_test() == LV_RES_OK);
  lv_obj_clean(screen);
}

// ---- 电量图标：4 格映射 + 位于时间右侧 + 不与其他顶栏元素重叠 ----
static void checkBatteryGauge(lv_disp_t *display) {
  // 纯逻辑：电压 -> 格数
  assert(BatteryGauge::cellsForVoltage(4.20f) == 4);
  assert(BatteryGauge::cellsForVoltage(4.05f) == 4);
  assert(BatteryGauge::cellsForVoltage(4.04f) == 3);
  assert(BatteryGauge::cellsForVoltage(3.85f) == 3);
  assert(BatteryGauge::cellsForVoltage(3.84f) == 2);
  assert(BatteryGauge::cellsForVoltage(3.65f) == 2);
  assert(BatteryGauge::cellsForVoltage(3.64f) == 1);
  assert(BatteryGauge::cellsForVoltage(3.45f) == 1);
  assert(BatteryGauge::cellsForVoltage(3.44f) == 0);
  assert(BatteryGauge::cellsForVoltage(3.30f) == 0);
  assert(BatteryGauge::cellsForVoltage(0.0f) == 0);
  assert(BatteryGauge::cellsForVoltage(-1.0f) == 0);
  assert(BatteryGauge::cellsForVoltage(NAN) == 0);
  // 单调性：电压越高格数不应减少
  uint8_t previous = 0;
  for (int step = 300; step <= 430; ++step) {
    const uint8_t cells = BatteryGauge::cellsForVoltage(step / 100.0f);
    assert(cells >= previous && cells <= BatteryGauge::Cells);
    previous = cells;
  }
  // 模拟电压随时长单调下降，且始终落在可显示范围
  float lastVoltage = 5.0f;
  for (uint32_t minutes = 0; minutes <= 600; minutes += 5) {
    const float voltage = BatteryGauge::simulatedVoltage(minutes * 60000u);
    assert(voltage <= lastVoltage && voltage >= 3.30f && voltage <= 4.20f);
    lastVoltage = voltage;
  }
  assert(BatteryGauge::critical(0) && !BatteryGauge::critical(1));

  lv_obj_t *screen = lv_disp_get_scr_act(display);
  lv_obj_clean(screen);
  DashboardView view;
  view.create(screen, [](lv_event_t *) {}, [](lv_event_t *) {});
  lv_obj_update_layout(screen);

  // 完整时分秒显示在右侧电池之前，顶栏文字使用统一的 14px 字号。
  lv_label_set_text(view.clock, "23:59:59");
  lv_obj_update_layout(screen);
  lv_area_t clockArea, barArea;
  lv_obj_get_coords(view.clock, &clockArea);
  lv_obj_get_coords(lv_obj_get_child(screen, 0), &barArea);
  assert(std::string(lv_label_get_text(view.clock)) == "23:59:59");
  const lv_obj_t *statusLabels[] = {
      view.network, view.timeStatus, view.sdIcon, view.settingsIcon, view.clock};
  for (const auto *label : statusLabels) {
    assert(lv_obj_get_style_text_font(label, 0) == &lv_font_jetbrains_mono_14);
    lv_point_t textSize;
    lv_txt_get_size(&textSize, lv_label_get_text(label),
                    lv_obj_get_style_text_font(label, 0),
                    lv_obj_get_style_text_letter_space(label, 0),
                    lv_obj_get_style_text_line_space(label, 0), LV_COORD_MAX,
                    LV_TEXT_FLAG_NONE);
    assert(textSize.x <= lv_obj_get_width(label));
    assert(textSize.y <= lv_obj_get_height(label));
  }
  lv_area_t networkArea, timeArea, sdArea, settingsArea;
  lv_obj_get_coords(view.network, &networkArea);
  lv_obj_get_coords(view.timeStatus, &timeArea);
  lv_obj_get_coords(view.sdIcon, &sdArea);
  lv_obj_get_coords(view.settingsIcon, &settingsArea);
  assert(networkArea.x2 < timeArea.x1);
  assert(timeArea.x2 < sdArea.x1);
  assert(sdArea.x2 < settingsArea.x1);
  assert(settingsArea.x2 < clockArea.x1);

  // 电池必须位于时钟右侧，靠顶栏右边缘且完整保留凸点。
  assert(view.batteryIcon_ != nullptr);
  lv_area_t batteryArea, batteryCapArea;
  lv_obj_get_coords(view.batteryIcon_, &batteryArea);
  lv_obj_get_coords(view.batteryCaps_, &batteryCapArea);
  assert(batteryArea.x1 > clockArea.x2);
  assert(batteryArea.y1 >= barArea.y1 && batteryArea.y2 <= barArea.y2);
  assert(batteryCapArea.x2 == barArea.x2 - DashboardView::BatteryRightMargin);
  assert(view.clock);

  // 电量格数：亮的格子数量必须精确等于传入的格数。
  const uint16_t lit = lv_color_make(226, 232, 240).full;
  for (uint8_t cells = 0; cells <= BatteryGauge::Cells; ++cells) {
    view.battery(cells, false);
    lv_obj_update_layout(screen);
    unsigned visible = 0;
    for (unsigned index = 0; index < BatteryGauge::Cells; ++index) {
      auto *cell = view.batteryCells_[index];
      assert(cell != nullptr);
      lv_area_t area;
      lv_obj_get_coords(cell, &area);
      // 每个格子都在外框内部
      assert(area.x1 >= batteryArea.x1 && area.x2 <= batteryArea.x2);
      assert(area.y1 >= batteryArea.y1 && area.y2 <= batteryArea.y2);
      if (lv_obj_get_style_bg_opa(cell, 0) == LV_OPA_COVER) {
        ++visible;
        assert(lv_obj_get_style_bg_color(cell, 0).full == lit);
      }
    }
    assert(visible == cells);
    lv_refr_now(display);
    assert(lv_mem_test() == LV_RES_OK);
  }

  // 0 格用红色告警
  view.battery(0, false);
  for (unsigned index = 0; index < BatteryGauge::Cells; ++index)
    assert(lv_obj_get_style_bg_opa(view.batteryCells_[index], 0) == LV_OPA_TRANSP);

  // 充电时全绿
  view.battery(2, true);
  lv_obj_update_layout(screen);
  assert(lv_obj_get_style_bg_color(view.batteryCells_[0], 0).full == lv_color_make(34, 197, 94).full);
  assert(lv_obj_get_style_bg_color(view.batteryCells_[1], 0).full == lv_color_make(34, 197, 94).full);
  assert(lv_obj_get_style_bg_opa(view.batteryCells_[2], 0) == LV_OPA_TRANSP);

  // 越界格数被收敛到 4
  view.battery(9, false);
  lv_obj_update_layout(screen);
  unsigned overflow = 0;
  for (unsigned index = 0; index < BatteryGauge::Cells; ++index)
    if (lv_obj_get_style_bg_opa(view.batteryCells_[index], 0) == LV_OPA_COVER) ++overflow;
  assert(overflow == BatteryGauge::Cells);

  lv_obj_clean(screen);
}

static void saveSensorFrame(const char *name) {
  lv_refr_now(lv_disp_get_default());
  const char *directory = std::getenv("UI_ARTIFACT_DIR");
  if (!directory) return;
  std::ofstream output(std::string(directory) + "/" + name, std::ios::binary);
  output << "P6\n240 320\n255\n";
  unsigned nonblank = 0;
  for (const auto pixel : framebuffer) {
    const uint16_t color = pixel.full;
    const char rgb[] = {char(((color >> 11) & 31) * 255 / 31),
                        char(((color >> 5) & 63) * 255 / 63), char((color & 31) * 255 / 31)};
    output.write(rgb, sizeof(rgb));
    if (color != framebuffer[0].full) ++nonblank;
  }
  assert(output.good() && nonblank > 500);
}

static void checkSensorViews(lv_obj_t *screen) {
  lv_obj_t *panel = lv_obj_create(screen);
  lv_obj_set_size(panel, 240, 320);
  lv_obj_set_style_pad_all(panel, 0, 0);
  lv_obj_set_style_border_width(panel, 0, 0);
  OnlineDevicesView online;
  online.create(panel);
  const char *names[] = {"功率计", "心率设备", "前臂六轴", "网络时间", "风速计", "车架六轴"};
  for (unsigned index = 0; index < OnlineDevicesView::kCount; ++index) {
    online.status(index, names[index], "已连接 / 响应正常");
    online.data(index, "P+42 R+13 Y-3  42ms");
  }
  lv_obj_update_layout(panel);
  assert(!lv_obj_has_flag(online.list(), LV_OBJ_FLAG_SCROLLABLE));
  assert(lv_obj_get_y(online.reading(5)) > lv_obj_get_y(online.reading(4)));
  lv_area_t listArea, finalRow;
  lv_obj_get_coords(online.list(), &listArea);
  lv_obj_get_coords(online.reading(5), &finalRow);
  assert(finalRow.y2 <= listArea.y2);
  saveSensorFrame("online-sensors-top.ppm");
  lv_obj_del(panel);
  panel = lv_obj_create(screen);
  lv_obj_set_size(panel, 240, 320);
  lv_obj_set_style_pad_all(panel, 0, 0);
  lv_obj_set_style_border_width(panel, 0, 0);
  PostureSetupView setup;
  setup.create(panel, [](lv_event_t *) {}, [](lv_event_t *) {});
  assert(setup.placement() == RiderPosture::Placement::Forearm);
  for (bool chinese : {false, true}) {
    setup.language(chinese);
    for (auto placement : {RiderPosture::Placement::Forearm, RiderPosture::Placement::UpperArm,
                           RiderPosture::Placement::BackStrap, RiderPosture::Placement::Helmet}) {
      setup.select(placement);
      assert(setup.placement() == placement);
      RiderPosture::Estimate estimate;
      estimate.state = RiderPosture::State::Forward;
      estimate.relativeDegrees = 42;
      setup.update(estimate);
      lv_obj_update_layout(panel);
      lv_area_t dropdown, button, result;
      lv_obj_get_coords(setup.dropdown(), &dropdown);
      lv_obj_get_coords(setup.button(0), &button);
      lv_obj_get_coords(setup.result(), &result);
      assert(!overlaps(dropdown, button) && !overlaps(dropdown, result));
      lv_point_t textSize;
      lv_txt_get_size(&textSize, lv_label_get_text(setup.result()), &lv_font_noto_sans_sc_14,
                      0, 0, 216, LV_TEXT_FLAG_NONE);
      assert(textSize.x <= 216 && textSize.y <= lv_obj_get_height(setup.result()));
    }
  }
  setup.language(true);
  setup.select(RiderPosture::Placement::BackStrap);
  RiderPosture::Estimate waiting;
  waiting.state = RiderPosture::State::NoData;
  setup.update(waiting);
  assert(lv_obj_has_state(setup.button(0), LV_STATE_DISABLED));
  assert(lv_obj_has_state(setup.button(1), LV_STATE_DISABLED));
  waiting.state = RiderPosture::State::NeedUpright;
  setup.update(waiting);
  assert(!lv_obj_has_state(setup.button(0), LV_STATE_DISABLED));
  assert(lv_obj_has_state(setup.button(1), LV_STATE_DISABLED));
  saveSensorFrame("posture-placement.ppm");
  lv_dropdown_open(setup.dropdown());
  saveSensorFrame("posture-placement-options.ppm");
  lv_dropdown_close(setup.dropdown());
  lv_obj_del(panel);
  std::cout << "PASS sensor UI: six no-scroll online rows, recent data, four placements, bilingual controls and result bounds\n";
}

int main(int argc, char **argv) {
  DashboardLayout layout;
  for (unsigned mask = 0; mask < 1024; ++mask) {
    DashboardLayout::Settings settings{};
    unsigned bits = mask;
    for (unsigned index = 0; index < 5; ++index) {
      settings.order[index] = index;
      settings.levels[index] = 1 + (bits & 3);
      bits >>= 2;
    }
    assert(layout.load(settings));
    unsigned maximum = 0;
    for (unsigned index = 0; index < 5; ++index) {
      maximum += layout.level(index) == 4;
      assert(layout.top(index) >= 44 && layout.top(index) + layout.height(layout.id(index)) <= 316);
    }
    assert(maximum <= 3);
  }
  layout.reset();
  const auto previous = layout.level(0);
  layout.press(0, 60, 100);
  assert(!layout.release(60, 149) && layout.level(0) == previous);
  layout.press(0, 60, 300);
  assert(!layout.release(60, 379) && layout.level(0) == previous);
  layout.press(0, 60, 500);
  assert(layout.release(60, 580) && layout.level(0) == previous + 1);
  assert(!layout.release(60, 600) && layout.level(0) == previous + 1);
  layout.press(0, 60, 650);
  assert(!layout.release(60, 759) && layout.level(0) == previous + 1);
  layout.press(0, 60, 680);
  assert(layout.release(60, 760) && layout.level(0) == 1);
  layout.press(0, 60, 1200);
  assert(!layout.dragReady(1799));
  assert(layout.dragReady(1800));
  assert(!layout.motion(285, 1799));
  assert(layout.motion(285, 1800));
  assert(layout.release(285, 1850) && layout.id(4) == 0);
  assert(!layout.dragReady(1850));
  const auto unchanged = layout.settings();
  layout.press(0, 60, 3450);
  assert(!layout.release(100, 3800));
  assert(!memcmp(&unchanged, &layout.settings(), sizeof(unchanged)));
  layout.press(0, 60, 3900);
  const auto heldLevel = layout.level(layout.id(0));
  assert(layout.release(60, 5900));
  assert(layout.level(layout.id(0)) == heldLevel % 4 + 1);
  assert(!layout.release(60, 6000));
  layout.press(0, 60, 6100);
  assert(layout.dragReady(6700));
  layout.cancel();
  assert(!layout.dragReady(6700));
  assert(!layout.release(60, 6750));
  layout.press(0, 20, 6600);
  assert(!layout.release(60, 7000));
  layout.press(0, 60, UINT32_MAX - 100);
  assert(layout.motion(200, 600));
  assert(!layout.release(20, 610));
  DashboardLayout wrap;
  wrap.press(0, 60, UINT32_MAX - 200);
  assert(wrap.release(60, 0));
  wrap.press(0, 60, 80);
  assert(!wrap.release(60, 179));
  wrap.press(0, 60, 100);
  assert(wrap.release(60, 180));
  wrap.press(0, 60, 500);
  wrap.press(0, 60, 540);
  assert(wrap.release(60, 580));
  DashboardLayout jitter;
  jitter.press(0, 60, 0, 100);
  assert(!jitter.motion(71, 60, 111));
  assert(jitter.release(71, 80, 111));
  jitter.press(0, 60, 1000, 100);
  assert(!jitter.motion(60, 1700, 112));
  assert(!jitter.dragReady(1700));
  assert(!jitter.release(60, 1800, 112));
  lv_init();
  SdActivity activity;
  assert(activity.state(false, 0) == SdActivity::State::Absent);
  assert(activity.state(true, 0) == SdActivity::State::Ready);
  activity.beginWrite();
  assert(activity.state(true, 100) == SdActivity::State::Writing);
  activity.endWrite(UINT32_MAX - 100, true);
  assert(activity.state(true, 200) == SdActivity::State::Writing);
  assert(activity.state(true, 600) == SdActivity::State::Ready);
  activity.beginWrite(); activity.endWrite(700, false);
  assert(activity.state(true, 2000) == SdActivity::State::Error);
  static lv_color_t buffer[240 * 20];
  static lv_disp_draw_buf_t drawBuffer;
  lv_disp_draw_buf_init(&drawBuffer, buffer, nullptr, 240 * 20);
  static lv_disp_drv_t driver;
  lv_disp_drv_init(&driver);
  driver.hor_res = 240; driver.ver_res = 320;
  driver.draw_buf = &drawBuffer; driver.flush_cb = flush;
  lv_disp_t *display = lv_disp_drv_register(&driver);
  assert(display);
  auto *screen = lv_disp_get_scr_act(display);
  lv_obj_set_style_pad_all(screen, 0, 0);
  lv_obj_set_style_bg_color(screen, lv_color_make(248, 250, 252), 0);
  checkDeviceNavigation();
  checkSensorViews(screen);
  checkGlyphs();
  if (argc == 2 && std::string(argv[1]) == "--sensor-views") return 0;
  lv_point_t legacyText;
  lv_txt_get_size(&legacyText, "WAITING FOR DATA", &lv_font_montserrat_14, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
  assert(legacyText.x > 84);
  std::cout << "Legacy waiting caption width: " << legacyText.x << " > 84px\n";
  AeroZoneView view;
  view.create(screen);
  for (unsigned repeat = 0; repeat < 100; ++repeat) {
    for (bool chinese : {false, true}) {
      view.setLanguage(chinese);
      for (unsigned zone = 0; zone <= 7; ++zone) {
        view.update(zone ? .31f : -1, zone);
        checkView(view);
        assert(std::string(lv_label_get_text(view.value())) == (zone ? "0.31" : "--"));
        assert(std::string(lv_label_get_text(view.title())) == (chinese ? "气动区间" : "AERO ZONE"));
        lv_refr_now(display);
        assert(lv_mem_test() == LV_RES_OK);
      }
    }
  }
  view.update(1931.24f, 1); checkView(view);
  view.update(NAN, 0); checkView(view);
  view.update(.31f, 4); lv_refr_now(display);
  view.setLanguage(false); lv_refr_now(display); saveFrame("aero-en.ppm");
  view.setLanguage(true); lv_refr_now(display); saveFrame("aero-zh.ppm");
  unsigned blue = 0;
  for (const auto color : framebuffer)
    if (color.full == lv_color_make(37, 99, 235).full) ++blue;
  assert(flushes > 100 && blue > 20);
  lv_obj_clean(screen);
  DashboardView dashboard;
  dashboard.create(screen, [](lv_event_t *) {}, [](lv_event_t *) {});
  lv_obj_update_layout(screen);
  lv_area_t sdArea, settingsArea;
  lv_obj_get_coords(dashboard.sdButton, &sdArea);
  lv_obj_get_coords(dashboard.settingsButton, &settingsArea);
  assert(lv_area_get_width(&sdArea) == 44 && lv_area_get_height(&sdArea) == 44);
  assert(lv_area_get_width(&settingsArea) == 44 && lv_area_get_height(&settingsArea) == 44);
  assert(!overlaps(sdArea, settingsArea) && sdArea.y2 < 44 && settingsArea.y2 < 44);
  for (unsigned mask = 0; mask < 1024; ++mask) {
    DashboardLayout::Settings settings{};
    for (unsigned index = 0; index < 5; ++index) {
      settings.order[index] = index;
      settings.levels[index] = 1 + ((mask >> (index * 2)) & 3);
    }
    assert(dashboard.load(settings));
    dashboard.setLanguage(mask & 1);
    dashboard.update(.31f, 4, 9.5f, true, -32768, 255);
    dashboard.heart(255, true);
    lv_obj_update_layout(screen);
    for (unsigned index = 0; index < 5; ++index) {
      lv_area_t rowArea, valueArea;
      lv_obj_get_coords(dashboard.row(index), &rowArea);
      lv_obj_get_coords(dashboard.reading(index), &valueArea);
      assert(rowArea.y1 >= 44 && rowArea.y2 < 316);
      assert(valueArea.x1 >= rowArea.x1 && valueArea.x2 <= rowArea.x2);
      assert(valueArea.y1 >= rowArea.y1 && valueArea.y2 <= rowArea.y2);
      assert(!overlaps(sdArea, rowArea) && !overlaps(settingsArea, rowArea));
      lv_point_t textSize;
      lv_txt_get_size(&textSize, lv_label_get_text(dashboard.reading(index)),
                      lv_obj_get_style_text_font(dashboard.reading(index), 0), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
      assert(textSize.x <= lv_obj_get_width(dashboard.reading(index)));
    }
    lv_refr_now(display);
    assert(lv_mem_test() == LV_RES_OK);
  }
  dashboard.storage(SdActivity::State::Writing, 250);
  lv_refr_now(display);
  dashboard.reset();
  dashboard.layout().move(0, 4);
  dashboard.layout().cycle(2);
  dashboard.retrySave();
  DashboardLayout::Settings saved;
  assert(!dashboard.takeSettings(saved));
  assert(dashboard.takeSettings(saved, true));
  assert(saved.order[4] == 0);
  for (unsigned index = 0; index < 5; ++index) assert(saved.levels[index] == dashboard.layout().level(index));
  assert(!dashboard.takeSettings(saved, true));
  dashboard.retrySave();
  assert(!dashboard.takeSettings(saved));
  lv_tick_inc(800);
  assert(dashboard.takeSettings(saved));
  DashboardView rebooted;
  lv_obj_clean(screen);
  rebooted.create(screen, [](lv_event_t *) {}, [](lv_event_t *) {});
  assert(rebooted.load(saved));
  for (unsigned position = 0; position < 5; ++position) {
    assert(rebooted.layout().id(position) == saved.order[position]);
    assert(rebooted.layout().level(position) == saved.levels[position]);
  }
  rebooted.reset();
  lv_obj_update_layout(screen);
  lv_indev_drv_t inputDriver;
  lv_indev_drv_init(&inputDriver);
  inputDriver.type = LV_INDEV_TYPE_POINTER;
  inputDriver.read_cb = readPointer;
  auto *pointer = lv_indev_drv_register(&inputDriver);
  assert(pointer);
  const auto pump = [pointer](uint32_t elapsed) {
    lv_tick_inc(elapsed);
    lv_timer_ready(pointer->driver->read_timer);
    lv_timer_handler();
  };
  pointerPressed = true;
  pump(20);
  pump(599);
  assert(lv_obj_get_style_bg_opa(rebooted.row(0), 0) == LV_OPA_TRANSP);
  pump(1);
  assert(lv_obj_get_style_bg_opa(rebooted.row(0), 0) == LV_OPA_COVER);
  assert(lv_obj_get_style_bg_color(rebooted.row(0), 0).full == lv_color_make(219, 234, 254).full);
  pointerPoint.y = 280;
  pump(20);
  assert(lv_obj_get_style_bg_color(rebooted.row(4), 0).full == lv_color_make(187, 247, 208).full);
  pointerPressed = false;
  pump(20);
  assert(rebooted.layout().id(4) == 0);
  for (unsigned index = 0; index < 5; ++index)
    assert(lv_obj_get_style_bg_opa(rebooted.row(index), 0) == LV_OPA_TRANSP);
  checkRecordingIndicator(display);
  checkBatteryGauge(display);
  std::cout << "PASS UI language: real LVGL, glyphs/icons, no overlap/clipping, values preserved, 1600 refreshes, heap integrity, nonblank pixels\n";
  std::cout << "PASS recording indicator: persists while recording, blinks green<->red, restores green on stop\n";
  std::cout << "PASS battery gauge: 4-cell mapping, cell count, right of clock, no overlap\n";
}