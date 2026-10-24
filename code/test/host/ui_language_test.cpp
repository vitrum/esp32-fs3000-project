#include "aero_zone_view.h"
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
  for (const char *name : {"/src/display_lcd.cpp", "/src/aero_zone_view.h"}) {
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
  lv_init();
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
  std::cout << "PASS UI language: real LVGL, glyphs/icons, no overlap/clipping, values preserved, 1600 refreshes, heap integrity, nonblank pixels\n";
}