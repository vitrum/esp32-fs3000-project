#include "aero_zone_view.h"
#include "dashboard_layout.h"
#include "dashboard_view.h"
#include <cassert>
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

static void checkGlyphs() {
  std::set<uint32_t> required;
  const std::regex literal(R"re("(?:\\.|[^"\\])*")re");
  for (const char *name : {"/src/display_lcd.cpp", "/src/aero_zone_view.h", "/src/dashboard_view.h"}) {
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
  for (const uint32_t codepoint : required) {
    lv_font_glyph_dsc_t glyph;
    assert(lv_font_get_glyph_dsc(&lv_font_noto_sans_sc_14, &glyph, codepoint, 0));
    assert(!glyph.is_placeholder);
    if (glyph.box_w && glyph.box_h) assert(lv_font_get_glyph_bitmap(&lv_font_noto_sans_sc_14, codepoint));
  }
  for (const char *symbol : {LV_SYMBOL_LEFT, LV_SYMBOL_SETTINGS, LV_SYMBOL_PLAY, LV_SYMBOL_SAVE}) {
    uint32_t index = 0;
    lv_font_glyph_dsc_t glyph;
    assert(lv_font_get_glyph_dsc(&lv_font_noto_sans_sc_14, &glyph, _lv_txt_encoded_next(symbol, &index), 0));
    assert(!glyph.is_placeholder);
  }
  std::cout << "CJK glyphs checked: " << required.size() << '\n';
}

int main() {
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
  checkGlyphs();
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
  std::cout << "PASS UI language: real LVGL, glyphs/icons, no overlap/clipping, values preserved, 1600 refreshes, heap integrity, nonblank pixels\n";
}