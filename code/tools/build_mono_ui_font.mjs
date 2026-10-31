// 用法：
//   node tools/build_mono_ui_font.mjs /path/to/JetBrainsMono-Medium.ttf
//
// 依赖：node >= 18、npx（通过 npx 临时拉取 lv_font_conv@1.5.3）。
// 说明：lv_font_conv 的 FreeType 走 WASM，Node 10/12 无法加载，必须用较新的 Node。
//
// 图标：LV_SYMBOL_* 用的是 FontAwesome 5 的 PUA 码点（0xF000 段），
// JetBrains Mono 里没有这些字形，因此额外合并 LVGL 自带的
// FontAwesome5-Solid+Brands+Regular.woff。图标不是等宽的，这是正常的。
// 码点集合从 LVGL 的 lv_symbol_def.h 解析得到（含 LV_SYMBOL_DUMMY 0xF8FF 会被剔除，
// 因为 FontAwesome 里没有该码点）。
//
// 许可证：JetBrains Mono 使用 SIL Open Font License 1.1（OFL-1.1），
// FontAwesome Free 5 的字体同样是 OFL-1.1（图标为 CC BY 4.0），
// 均可免费商用、可嵌入固件。完整文本见 lib/jetbrains_mono/OFL.txt。

import { readFileSync, writeFileSync, existsSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import os from 'node:os';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const font = process.argv[2];
if (!font) {
  throw new Error('Usage: node tools/build_mono_ui_font.mjs /path/to/JetBrainsMono-Medium.ttf');
}

// 冻结源字体版本：JetBrains Mono v2.304 的 Medium 字重。
const sourceHash = createHash('sha256').update(readFileSync(font)).digest('hex');
if (sourceHash !== '31c92d01a8a08528b718a43addf0ad3df0af2ca4b7b3290a452f70f358e14d3d') {
  throw new Error(
    'Unexpected font source SHA256; review the source/version before regenerating\n' +
    `got: ${sourceHash}`);
}

// 从 LVGL 的 lv_symbol_def.h 解析出所有 LV_SYMBOL_* 的码点。
const lvglDir = path.join(root, '.pio/libdeps/esp32s3/lvgl');
const symbolDef = path.join(lvglDir, 'src/font/lv_symbol_def.h');
const faFont = path.join(lvglDir, 'scripts/built_in_font/FontAwesome5-Solid+Brands+Regular.woff');
if (!existsSync(symbolDef) || !existsSync(faFont)) {
  throw new Error('LVGL 依赖不完整；先跑一次 pio run 生成 .pio/libdeps/esp32s3/lvgl');
}

const iconCodepoints = [];
const defSource = readFileSync(symbolDef, 'utf8');
for (const match of defSource.matchAll(/#define\s+LV_SYMBOL_[A-Z_0-9]+\s+"((?:\\x[0-9A-Fa-f]{2})+)"/g)) {
  const bytes = [...match[1].matchAll(/\\x([0-9A-Fa-f]{2})/g)]
    .map((part) => parseInt(part[1], 16));
  const codepoint = Buffer.from(bytes).toString('utf8').codePointAt(0);
  // 只取 FontAwesome 的 PUA 私有区（0xF000+）；0xF8FF(LV_SYMBOL_DUMMY) 不存在于字体中。
  if (codepoint >= 0xf000 && codepoint <= 0xf8fe && !iconCodepoints.includes(codepoint)) {
    iconCodepoints.push(codepoint);
  }
}
iconCodepoints.sort((a, b) => a - b);
const iconRange = iconCodepoints.map((c) => '0x' + c.toString(16)).join(',');

const sizes = [14, 16, 18, 20, 28, 40, 48];
const tmp = path.join(os.tmpdir(), `jbm-${process.pid}`);
execFileSync('mkdir', ['-p', tmp]);

for (const size of sizes) {
  const raw = path.join(tmp, `jbm_${size}.raw.c`);
  execFileSync('npx', ['--yes', '--package', 'lv_font_conv@1.5.3', 'lv_font_conv',
    '--size', String(size), '--bpp', '4', '--format', 'lvgl',
    '--font', font, '--range', '0x20-0x7E',
    '--font', faFont, '--range', iconRange,
    '--no-kerning', '--no-compress', '--lv-include', 'lvgl.h',
    '--lv-font-name', `lv_font_jetbrains_mono_${size}`, '-o', raw], { stdio: 'inherit', cwd: root });

  // 去掉 lv_font_conv 自带的 banner 注释和 include 块，换上项目自己的说明。
  const body = readFileSync(raw, 'utf8')
    .replace(/^\/\*{10,}[\s\S]*?\*\/\n/, '')
    .replace(/#ifdef LV_LVGL_H_INCLUDE_SIMPLE[\s\S]*?#endif\n\n/, '');

  const banner = `/**
 * LVGL 字模：JetBrains Mono Medium ${size}px + FontAwesome 5 图标
 *
 * 拉丁字符 0x20-0x7E 全部等宽（同一 adv_w），实时数值刷新不抖动。
 * 图标为 FontAwesome 5 PUA 码点（LV_SYMBOL_* 使用），宽度各自独立。
 *
 * 许可证：
 *   - JetBrains Mono v2.304 —— SIL Open Font License 1.1（OFL-1.1）
 *   - FontAwesome Free 5     —— SIL OFL-1.1（字体）+ CC BY 4.0（图标）
 *   两者均免费、可商用、可嵌入固件；详见 lib/jetbrains_mono/OFL.txt
 *   与 https://fontawesome.com/license/free
 *
 * 生成：tools/build_mono_ui_font.mjs（lv_font_conv 1.5.3）
 * 源字体 SHA256: ${sourceHash}
 */
#include "lvgl.h"

`;

  writeFileSync(path.join(root, `src/jetbrains_mono_${size}.c`), banner + body.trim() + '\n');
  console.log(`Wrote src/jetbrains_mono_${size}.c`);
}

execFileSync('rm', ['-rf', tmp]);
console.log(`Font source SHA256: ${sourceHash}`);
console.log(`Icons merged: ${iconCodepoints.length}`);
