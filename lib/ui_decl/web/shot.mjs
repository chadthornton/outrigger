// Renders a screen with the WebAssembly build, outside a browser, and saves
// the PNG and raw RGB565 frame the wasm module wrote. `make ui-decl-compare`
// diffs these against `make ui-shots`'.
//
//   node lib/ui_decl/web/shot.mjs <ui_decl.js> <screens dir> <screen> <w> <h> <out dir>
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { createRequire } from 'node:module';
import { resolve } from 'node:path';

const [js, dir, screen, w, h, out] = process.argv.slice(2);
const UiDecl = createRequire(import.meta.url)(resolve(js));
const m = await UiDecl();
const read = (f) => readFileSync(`${dir}/${f}`, 'utf8');
const W = Number(w), H = Number(h);

const ok = m.ccall('udw_render', 'number', ['number', 'number', 'string', 'string', 'string'],
  [W, H, read('theme.json'), read(`${screen}.json`), read(`${screen}.data.json`)]);
if (!ok) {
  console.error(`ui_decl web: ${m.UTF8ToString(m._udw_error())}`);
  process.exit(1);
}
mkdirSync(out, { recursive: true });
const name = `${screen}_${W}x${H}`;
m.ccall('udw_write_png', 'number', ['string'], [`/${name}.png`]);
writeFileSync(`${out}/${name}.png`, m.FS.readFile(`/${name}.png`));
const p = m._udw_rgb565();
writeFileSync(`${out}/${name}.rgb565`, Buffer.from(m.HEAPU8.subarray(p, p + W * H * 2)));
const problems = m._udw_problems();
console.log(`  ${problems ? 'FAIL' : 'ok  '} web ${name}: ${problems} layout problems`);
process.exit(problems ? 1 : 0);
