#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>
#include <stdbool.h>

#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0
#define LV_MEM_CUSTOM 0
// 内置堆大小：默认仅 48KB，本 UI 有 ~15 个全屏面板 + 多字号/中文大字体，
// 切换新界面首次全量绘制时会申请大量临时内存，48KB 不足导致 lv_mem_alloc
// 失败 -> 绘制异常 -> 界面卡死。增大到 128KB。
#define LV_MEM_SIZE (128 * 1024)
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
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_40 1
#define LV_FONT_DEFAULT &lv_font_montserrat_18
#define LV_HOR_RES_MAX 240
#define LV_VER_RES_MAX 320
#define LV_DISP_DEF_REFR_PERIOD 30
#define LV_INDEV_DEF_READ_PERIOD 10
#define LV_LAYER_SIMPLE_BUF_SIZE (240 * 320)
#define LV_USE_USER_DATA 1

#endif
