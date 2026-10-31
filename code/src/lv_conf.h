#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>
#include <stdbool.h>

#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0
#define LV_MEM_CUSTOM 0
#define LV_MEM_SIZE (128 * 1024)
#define LV_MEM_POOL_INCLUDE <esp_heap_caps.h>
#define LV_MEM_POOL_ALLOC(size) heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0
#define LV_USE_LOG 0
#define LV_USE_FS_STDIO 0
#define LV_USE_SNAPSHOT 0
#define LV_USE_THEME_DEFAULT 1
#define LV_USE_FONT_COMPRESSED 1
#define LV_LARGE_NAME 0
#define LV_USE_ASSERT_NULL 0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ 0
#define LV_USE_ASSERT_STYLE 0
#define LV_FONT_MONTSERRAT_14 0
#define LV_FONT_MONTSERRAT_16 0
#define LV_FONT_MONTSERRAT_18 0
#define LV_FONT_MONTSERRAT_20 0
#define LV_FONT_MONTSERRAT_28 0
#define LV_FONT_MONTSERRAT_40 0
#define LV_FONT_MONTSERRAT_48 0

// ---- 英文/数字等宽字体（JetBrains Mono, OFL-1.1）----
// 源文件 src/jetbrains_mono_<size>.c；字重 Medium，Latin 0x20-0x7E。
// 等宽的好处：实时数值刷新时数字不会左右抖动。
#define LV_FONT_JETBRAINS_MONO_14 1
#define LV_FONT_JETBRAINS_MONO_16 1
#define LV_FONT_JETBRAINS_MONO_18 1
#define LV_FONT_JETBRAINS_MONO_20 1
#define LV_FONT_JETBRAINS_MONO_28 1
#define LV_FONT_JETBRAINS_MONO_40 1
#define LV_FONT_JETBRAINS_MONO_48 1
// LV_FONT_DEFAULT 在 lv_conf.h 展开处就要能解析符号，这里补声明。
// 字体实际定义在 src/jetbrains_mono_<size>.c。
// 注意：此处早于 lv_font.h，LV_FONT_DECLARE 宏尚不可用，直接写 extern。
extern const struct _lv_font_t lv_font_jetbrains_mono_18;
#define LV_FONT_DEFAULT &lv_font_jetbrains_mono_18
#define LV_HOR_RES_MAX 240
#define LV_VER_RES_MAX 320
#define LV_DISP_DEF_REFR_PERIOD 30
#define LV_INDEV_DEF_READ_PERIOD 10
#define LV_LAYER_SIMPLE_BUF_SIZE (240 * 320)
#define LV_USE_USER_DATA 1

#endif

