#pragma once

#include <lvgl.h>

// 英文/数字主字体：JetBrains Mono Medium（等宽，Latin 0x20-0x7E）
// 定义在 src/jetbrains_mono_<size>.c；许可证 SIL OFL-1.1（lib/jetbrains_mono/OFL.txt）。
// 重新生成：node tools/build_mono_ui_font.mjs <JetBrainsMono-Medium.ttf>

LV_FONT_DECLARE(lv_font_jetbrains_mono_14);
LV_FONT_DECLARE(lv_font_jetbrains_mono_16);
LV_FONT_DECLARE(lv_font_jetbrains_mono_18);
LV_FONT_DECLARE(lv_font_jetbrains_mono_20);
LV_FONT_DECLARE(lv_font_jetbrains_mono_28);
LV_FONT_DECLARE(lv_font_jetbrains_mono_40);
LV_FONT_DECLARE(lv_font_jetbrains_mono_48);

