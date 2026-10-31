// What the installer may write, decided before anything is written. Pure: no
// Web Serial, no esptool-js, so `make test` runs it on the Mac
// (tests/installer/plan.test.mjs).
//
// The page writes the same four parts as `make flash`, at offsets from this
// file's own list, never from the manifest. esptool-js erases whole 4 KiB
// sectors before it writes, so each part is checked by its sector-rounded end
// (the same as its length while every limit is sector-aligned, as today).
// NVS (settings, Wi-Fi) is never written. The store (every FM-1 backup) is
// written only to set one up on a board whose store region is factory-blank
// (store.js), and only inside the new table's store.

export const SECTOR = 0x1000;

// offset, name, the first offset it must not reach
export const PARTS = [
  { offset: 0x0, name: "startup code", limit: 0x8000 },
  { offset: 0x8000, name: "partition table", limit: 0x9000 },
  { offset: 0xe000, name: "boot selector", limit: 0x10000 },
  { offset: 0x10000, name: "program", limit: null }, // app0's end, from the new table
];

// The layout every install keeps; tools/build_site.py's FIXED and check_table
// say the same, and both run tests/installer/tables.json.
const FIXED = { nvs: [0x9000, 0x5000], otadata: [0xe000, 0x2000] };
const APP0 = [0x10000, 0x300000];

const hex = (n) => "0x" + n.toString(16);
const roundUp = (n) => Math.ceil(n / SECTOR) * SECTOR;

// An ESP-IDF partition table: 32-byte entries starting AA 50, ended by the MD5
// entry (EB EB) or erased flash. Returns name -> [offset, size].
export function parseTable(bytes) {
  const table = {};
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  for (let at = 0; at + 32 <= bytes.length; at += 32) {
    if (bytes[at] !== 0xaa || bytes[at + 1] !== 0x50) break;
    const raw = bytes.subarray(at + 12, at + 28);
    const end = raw.indexOf(0);
    const name = new TextDecoder().decode(end < 0 ? raw : raw.subarray(0, end));
    table[name] = [view.getUint32(at + 4, true), view.getUint32(at + 8, true)];
  }
  return table;
}

// Problems with a partition table about to be written (empty when fine).
export function checkTable(table, flashBytes) {
  const problems = [];
  for (const [name, want] of Object.entries(FIXED)) {
    const got = table[name];
    if (!got || got[0] !== want[0] || got[1] !== want[1]) {
      problems.push(`${name} is ${got ? got.map(hex).join("+") : "missing"}, must stay at ${want.map(hex).join("+")}`);
    }
  }
  const app0 = table.app0;
  if (!app0 || app0[0] !== APP0[0] || app0[1] < APP0[1]) problems.push("app0 must start at 0x10000 with at least 3 MB");
  if (table.store) {
    const [off, size] = table.store;
    if (off % SECTOR || off + size > flashBytes) problems.push("store is misaligned or runs past the flash");
  }
  const ranges = Object.entries(table).map(([name, [off, size]]) => [off, off + size, name]).sort((a, b) => a[0] - b[0]);
  for (let i = 1; i < ranges.length; i++) {
    if (ranges[i][0] < ranges[i - 1][1]) problems.push(`${ranges[i - 1][2]} overlaps ${ranges[i][2]}`);
  }
  return problems;
}

// parts: [{offset, data: Uint8Array}] from the build. boardTable: the 0xC00
// bytes the board holds at 0x8000 (all 0xFF on a blank board). storeWrites:
// [{offset, data}] to set up a store, allowed only with storeBlank (the
// board's store region read back as all 0xFF).
// Returns {ok, problems, writes, store}: writes is empty unless ok; store is
// the backup store the install keeps ({offset, size}), or null if the board
// has none.
export function planInstall({ parts, boardTable, flashBytes, storeWrites = [], storeBlank = false }) {
  const problems = [];
  const byOffset = new Map();
  for (const p of parts) {
    if (!PARTS.some((k) => k.offset === p.offset)) problems.push(`refusing a part at ${hex(p.offset)}: not one of the four install offsets`);
    else byOffset.set(p.offset, p.data);
  }
  for (const k of PARTS) {
    const data = byOffset.get(k.offset);
    if (!data) problems.push(`the ${k.name} (${hex(k.offset)}) is missing`);
    else if (!data.length) problems.push(`the ${k.name} (${hex(k.offset)}) is empty`);
  }

  const tableBytes = byOffset.get(0x8000);
  const newTable = tableBytes ? parseTable(tableBytes) : {};
  problems.push(...checkTable(newTable, flashBytes));

  for (const k of PARTS) {
    const data = byOffset.get(k.offset);
    if (!data || !data.length) continue;
    const end = k.offset + roundUp(data.length);
    const limit = k.limit ?? (newTable.app0 ? newTable.app0[0] + newTable.app0[1] : k.offset);
    if (end > limit) {
      const into = k.limit === 0x9000 ? "nvs (settings)" : k.limit === null ? "app0" : hex(k.limit);
      problems.push(`the ${k.name} at ${hex(k.offset)} would erase up to ${hex(end)}, past ${into}`);
    }
  }

  // The board's own table decides where its backups are. The new table must
  // keep that store exactly where it is, or the install would orphan it.
  const board = parseTable(boardTable);
  let store = null;
  if (board.store) {
    const [off, size] = board.store;
    store = { offset: off, size };
    const next = newTable.store;
    if (!next || next[0] !== off || next[1] !== size) {
      problems.push(`this board's backup store is at ${hex(off)}+${hex(size)}; this build would ` +
        (next ? `move it to ${hex(next[0])}+${hex(next[1])}` : "drop it"));
    }
  }

  if (storeWrites.length) {
    const range = newTable.store;
    if (!storeBlank) problems.push("refusing to write a store: the store region wasn't found blank");
    else if (!range) problems.push("refusing to write a store: this build's table has none");
    for (const w of storeWrites) {
      if (!range) break;
      const [off, size] = range;
      if (w.offset % SECTOR || w.offset < off || w.offset + roundUp(w.data.length) > off + size) {
        problems.push(`refusing a store write at ${hex(w.offset)}: outside the store or not on a block`);
      }
    }
  }

  const ok = problems.length === 0;
  const writes = ok ? [...PARTS.map((k) => ({ offset: k.offset, name: k.name, data: byOffset.get(k.offset) })),
    ...storeWrites.map((w) => ({ offset: w.offset, name: "store", data: w.data }))] : [];
  return { ok, problems, writes, store };
}
