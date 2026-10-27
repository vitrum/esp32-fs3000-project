import { readFileSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const font = process.argv[2];
if (!font) throw new Error('Usage: node tools/build_ui_font.mjs /path/to/NotoSansCJKsc-Regular.otf');
const sourceHash = createHash('sha256').update(readFileSync(font)).digest('hex');
if (sourceHash !== '2c76254f6fc379fddfce0a7e84fb5385bb135d3e399294f6eeb6680d0365b74b') {
  throw new Error('Unexpected font source SHA256; review the source/version before regenerating');
}
const files = ['src/display_lcd.cpp', 'src/aero_zone_view.h', 'src/dashboard_view.h'];
const characters = new Set();
for (const file of files) {
  const source = readFileSync(path.join(root, file), 'utf8');
  for (const literal of source.matchAll(/"(?:\\.|[^"\\])*"/g)) {
    for (const character of literal[0]) {
      if (/[\p{Script=Han}\u3000-\u303f\uff00-\uffef]/u.test(character)) characters.add(character);
    }
  }
}
const symbols = [...characters].sort((first, second) => first.codePointAt(0) - second.codePointAt(0)).join('');
console.log(`CJK glyphs: ${characters.size}`);
console.log(`Font source SHA256: ${sourceHash}`);
execFileSync('npx', ['--yes', '--package', 'lv_font_conv@1.5.3', 'lv_font_conv',
  '--size', '14', '--bpp', '4', '--format', 'lvgl', '--font', font,
  '--symbols', symbols, '--no-kerning', '--no-compress', '--lv-include', 'lvgl.h',
  '--lv-font-name', 'lv_font_noto_sans_sc_14', '--lv-fallback', 'lv_font_montserrat_14',
  '-o', path.join(root, 'src/noto_sans_sc_14.c')], { stdio: 'inherit', cwd: root });