# 字体：JetBrains Mono + FontAwesome 5（LCD 英文/数字等宽字体）

LCD 界面上的英文标签与实时数字统一使用**等宽字体**，刷新数值时字符宽度一致，
数字不会左右抖动。

| 项目 | 内容 |
| ---- | ---- |
| 主字体 | JetBrains Mono Medium v2.304 |
| 上游 | https://github.com/JetBrains/JetBrainsMono |
| 主字体许可 | **SIL Open Font License 1.1（OFL-1.1）** —— 免费、可商用、可嵌入固件、可再分发 |
| 图标字体 | FontAwesome Free 5（随 LVGL 提供，`lvgl/scripts/built_in_font/`） |
| 图标许可 | 字体 OFL-1.1；图标 CC BY 4.0（https://fontawesome.com/license/free） |
| 覆盖范围 | 拉丁 0x20–0x7E（等宽）+ FontAwesome PUA 图标（`LV_SYMBOL_*`） |
| 字号 | 14 / 16 / 18 / 20 / 28 / 40 / 48 px，4 bpp，未压缩 |

## 为什么还要合并 FontAwesome

`LV_SYMBOL_GPS`、`LV_SYMBOL_SAVE` 这些图标是 FontAwesome 的私有区码点（0xF000 段），
JetBrains Mono 里没有。仅替换 Montserrat 会让所有图标变成空白，因此生成时
**额外合并 LVGL 自带的 FontAwesome 5**。拉丁字符仍全部等宽，图标宽度各自独立。

## 生成产物

- `src/jetbrains_mono_14.c` … `src/jetbrains_mono_48.c`（每个字号一个文件）
- `src/jetbrains_mono_latin.h`（`LV_FONT_DECLARE` 声明）
- `src/lv_conf.h` 中启用 `LV_FONT_JETBRAINS_MONO_*`，并把 `LV_FONT_DEFAULT` 指向 18px

## 重新生成

```sh
# 需要 Node >= 18（lv_font_conv 的 FreeType 走 WASM，Node 10/12 无法加载）
node tools/build_mono_ui_font.mjs /path/to/JetBrainsMono-Medium.ttf
```

脚本会校验源字体 SHA256（`31c92d01…14d3d`），从 LVGL 的 `lv_symbol_def.h`
自动解析图标码点，并更新上述所有文件。

## 许可证文本

- 本目录 `OFL.txt`：JetBrains Mono 的 OFL-1.1 全文 + 派生说明
- `src/NotoSansCJK-OFL.txt`：中文回退字体（思源黑体）的 OFL-1.1
- FontAwesome：https://fontawesome.com/license/free
