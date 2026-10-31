// The page's write planner (site/installer/plan.js) against the shared cases in
// tables.json, which tools/build_site.py's check_table also runs
// (tests/installer/test_tables.py). Run by `make test`: node --test tests/installer/
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync, readdirSync } from "node:fs";
import { planInstall, checkTable, parseTable } from "../../site/installer/plan.js";

const root = new URL("../../", import.meta.url);
const cases = JSON.parse(readFileSync(new URL("tests/installer/tables.json", root)));

function ours() {
  const table = {};
  for (let line of readFileSync(new URL("firmware/waveshare_169/partitions.csv", root), "utf8").split("\n")) {
    line = line.split("#")[0].trim();
    if (!line) continue;
    const [name, type, , off, size] = line.split(",").map((f) => f.trim());
    table[name] = [Number(off), Number(size), type];
  }
  return table;
}

function resolve(spec) {
  if (spec === null) return null;
  if (spec === "ours") return ours();
  const { _replace, ...entries } = spec;
  return { ...(_replace ? {} : ours()), ...entries };
}

// An ESP-IDF partition table as the chip stores it: 32-byte entries, then an
// MD5 entry, then 0xFF.
function encode(table, length = 0xc00) {
  const out = new Uint8Array(length).fill(0xff);
  if (!table) return out;
  const view = new DataView(out.buffer);
  Object.entries(table).forEach(([name, [off, size, type]], i) => {
    const at = i * 32;
    out.set([0xaa, 0x50, type === "app" || name.startsWith("app") ? 0 : 1, 0], at);
    view.setUint32(at + 4, off, true);
    view.setUint32(at + 8, size, true);
    out.fill(0, at + 12, at + 32);
    out.set(new TextEncoder().encode(name), at + 12);
  });
  out.set([0xeb, 0xeb], Object.keys(table).length * 32);
  return out;
}

function partsFor(c) {
  const lengths = { ...cases.default_parts, ...(c.parts || {}) };
  const newTable = resolve(c.new_table);
  const parts = [];
  for (const [off, len] of Object.entries(lengths)) {
    if (len === null) continue;
    const offset = Number(off);
    let data = new Uint8Array(len).fill(0x5a);
    if (offset === 0x8000) data = encode(newTable, len);
    parts.push({ offset, data });
  }
  return parts;
}

for (const c of cases.cases) {
  test(`plan: ${c.name}`, () => {
    const plan = planInstall({
      parts: partsFor(c),
      boardTable: encode(resolve(c.board_table)),
      flashBytes: cases.flash_bytes,
    });
    if (c.expect === "ok") {
      assert.deepEqual(plan.problems, []);
      assert.equal(plan.ok, true);
      assert.deepEqual(plan.writes.map((w) => w.offset), [0x0, 0x8000, 0xe000, 0x10000]);
    } else {
      assert.equal(plan.ok, false, `expected a refusal mentioning "${c.expect}"`);
      assert.equal(plan.writes.length, 0);
      assert.match(plan.problems.join("; "), new RegExp(c.expect.replace(/[.*+?^${}()|[\]\\]/g, "\\$&"), "i"));
    }
  });

  test(`checkTable: ${c.name}`, () => {
    const table = Object.fromEntries(Object.entries(resolve(c.new_table)).map(([n, [o, s]]) => [n, [o, s]]));
    assert.equal(checkTable(table, cases.flash_bytes).length === 0, c.table_ok);
  });
}

test("parseTable reads the entries and stops at the MD5 entry", () => {
  const table = parseTable(encode(ours()));
  assert.deepEqual(table.store, [0x3d0000, 0xc20000]);
  assert.deepEqual(Object.keys(table), ["nvs", "otadata", "app0", "recovery", "store", "coredump"]);
  assert.deepEqual(parseTable(encode(null)), {});
});

test("a board with our store keeps it, and the plan says so", () => {
  const c = cases.cases.find((x) => x.name === "board already has our layout");
  const plan = planInstall({ parts: partsFor(c), boardTable: encode(ours()), flashBytes: cases.flash_bytes });
  assert.deepEqual(plan.store, { offset: 0x3d0000, size: 0xc20000 });
});

test("a blank board has no store to keep", () => {
  const c = cases.cases.find((x) => x.name === "blank board");
  const plan = planInstall({ parts: partsFor(c), boardTable: encode(null), flashBytes: cases.flash_bytes });
  assert.equal(plan.store, null);
});

test("the installer never erases: no erase call anywhere in site/installer", () => {
  const dir = new URL("site/installer/", root);
  for (const file of readdirSync(dir).filter((f) => f.endsWith(".js"))) {
    const src = readFileSync(new URL(file, dir), "utf8");
    assert.doesNotMatch(src, /eraseFlash|eraseAll\s*:\s*true|eraseRegion|erase_flash/, `${file} mentions an erase`);
  }
});
