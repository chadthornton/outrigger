// Setting up the store on a board that has none (site/installer/store.js),
// and the planner's one permission to write it (plan.js).
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync, existsSync } from "node:fs";
import { createHash } from "node:crypto";
import { storeAction, spliceStore, fetchLoader } from "../../site/installer/store.js";
import { planInstall } from "../../site/installer/plan.js";

const root = new URL("../../", import.meta.url);
const sha256 = (b) => createHash("sha256").update(b).digest("hex");

test("storeAction: a build without the loader leaves the store alone", () => {
  assert.equal(storeAction({ runsLoader: false, hasStore: false, blank: true }), "none");
});

test("storeAction: an existing store is kept, never written", () => {
  assert.equal(storeAction({ runsLoader: true, hasStore: true, blank: false }), "keep");
  assert.equal(storeAction({ runsLoader: true, hasStore: true, blank: true }), "keep");
});

test("storeAction: a factory-blank store region gets a store", () => {
  assert.equal(storeAction({ runsLoader: true, hasStore: false, hadStorePartition: true, blank: true }), "create");
});

test("storeAction: our store partition with data but no readable store stops the install", () => {
  assert.equal(storeAction({ runsLoader: true, hasStore: false, hadStorePartition: true, blank: false }), "unreadable");
});

// Chad, 2026-10-31: a board that ran other firmware (no store in its table)
// has that firmware's leftovers where the store goes, not FM-1 backups.
test("storeAction: a board from other firmware gets a store over its leftovers", () => {
  assert.equal(storeAction({ runsLoader: true, hasStore: false, hadStorePartition: false, blank: false }), "create");
  assert.equal(storeAction({ runsLoader: true, hasStore: false, hadStorePartition: false, blank: true }), "create");
});

// The real template and the splice tools/store_template.py checked by mounting
// it (tests/installer/test_store_template.py runs first in make test).
const templatePath = new URL("build/store-template.json", root);
const expectedPath = new URL("build/store-spliced.sha256", root);
const blobPath = new URL("tests/fixtures/wl82loader.bin", root);
const haveReal = existsSync(templatePath) && existsSync(expectedPath) && existsSync(blobPath);

test("spliceStore matches the splice the Mac mounted and read back", { skip: !haveReal && "run test_store_template.py first" }, () => {
  const template = JSON.parse(readFileSync(templatePath));
  const blob = new Uint8Array(readFileSync(blobPath));
  const blocks = spliceStore(template, blob);
  const joined = Buffer.concat(blocks.flatMap(({ offset, data }) => [Buffer.from(new Uint32Array([offset]).buffer), Buffer.from(data)]));
  assert.equal(sha256(joined), readFileSync(expectedPath, "utf8").trim());
});

const tiny = {
  size: 0x3000,
  blob_size: 8,
  loader: { url: "https://example.invalid/wl82loader.bin", sha256: sha256(Buffer.from("LOADERAB")) },
  blocks: [{ offset: 0x1000, hex: "00".repeat(0x1000) }],
  extents: [[0x1004, 0, 4], [0x1010, 4, 4]],
};

test("spliceStore puts each piece of the loader at its extent, in a copy of the block", () => {
  const [block] = spliceStore(tiny, new TextEncoder().encode("LOADERAB"));
  assert.equal(block.offset, 0x1000);
  assert.equal(new TextDecoder().decode(block.data.subarray(4, 8)), "LOAD");
  assert.equal(new TextDecoder().decode(block.data.subarray(0x10, 0x14)), "ERAB");
  assert.equal(tiny.blocks[0].hex.slice(8, 16), "00000000");
});

const ok = (bytes) => async () => ({ ok: true, arrayBuffer: async () => Uint8Array.from(bytes).buffer });

test("fetchLoader returns the loader when its SHA-256 matches", async () => {
  const got = await fetchLoader(tiny, ok(Buffer.from("LOADERAB")));
  assert.equal(new TextDecoder().decode(got), "LOADERAB");
});

test("fetchLoader gives GitHub a deadline", async () => {
  let signal;
  await fetchLoader(tiny, async (url, opts) => { signal = opts && opts.signal; return ok(Buffer.from("LOADERAB"))(); });
  assert.ok(signal instanceof AbortSignal);
});

test("fetchLoader says why when the download is wrong or fails", async () => {
  await assert.rejects(fetchLoader(tiny, ok(Buffer.from("LOADERXX"))), /didn't match/);
  await assert.rejects(fetchLoader(tiny, async () => ({ ok: false, status: 404 })), /404/);
  await assert.rejects(fetchLoader(tiny, async () => { throw new TypeError("Failed to fetch"); }), /couldn't reach GitHub/i);
});

// The planner: store writes only onto a blank store region, inside the new table's store.
function ourTable() {
  const out = new Uint8Array(0xc00).fill(0xff);
  const view = new DataView(out.buffer);
  [["nvs", 1, 0x9000, 0x5000], ["otadata", 1, 0xe000, 0x2000], ["app0", 0, 0x10000, 0x300000],
    ["recovery", 0, 0x310000, 0xc0000], ["store", 1, 0x3d0000, 0xc20000], ["coredump", 1, 0xff0000, 0x10000]]
    .forEach(([name, type, off, size], i) => {
      out.fill(0, i * 32, i * 32 + 32);
      out.set([0xaa, 0x50, type, 0], i * 32);
      view.setUint32(i * 32 + 4, off, true);
      view.setUint32(i * 32 + 8, size, true);
      out.set(new TextEncoder().encode(name), i * 32 + 12);
    });
  return out;
}
const parts = () => [
  { offset: 0x0, data: new Uint8Array(20000).fill(1) },
  { offset: 0x8000, data: ourTable() },
  { offset: 0xe000, data: new Uint8Array(0x2000) },
  { offset: 0x10000, data: new Uint8Array(900000) },
];
const storeBlock = (rel) => ({ offset: 0x3d0000 + rel, data: new Uint8Array(0x1000).fill(7) });

test("plan: store blocks are written after the four parts when the store region is blank", () => {
  const plan = planInstall({ parts: parts(), boardTable: ourTable(), flashBytes: 16 << 20,
    storeWrites: [storeBlock(0), storeBlock(0x1000)], storeWritable: true });
  assert.deepEqual(plan.problems, []);
  assert.deepEqual(plan.writes.map((w) => w.offset), [0x0, 0x8000, 0xe000, 0x10000, 0x3d0000, 0x3d1000]);
});

test("plan: no store writes unless prepareStore cleared the region", () => {
  const plan = planInstall({ parts: parts(), boardTable: ourTable(), flashBytes: 16 << 20,
    storeWrites: [storeBlock(0)], storeWritable: false });
  assert.equal(plan.ok, false);
  assert.match(plan.problems.join("; "), /store/);
});

test("plan: store writes go through once prepareStore cleared the region", () => {
  const plan = planInstall({ parts: parts(), boardTable: ourTable(), flashBytes: 16 << 20,
    storeWrites: [storeBlock(0)], storeWritable: true });
  assert.deepEqual(plan.problems, []);
});

test("plan: a store write outside the store, or not on a block, is refused", () => {
  for (const w of [{ offset: 0x3cf000, data: new Uint8Array(0x1000) }, { offset: 0x3d0800, data: new Uint8Array(0x1000) },
    { offset: 0x3d0000 + 0xc20000 - 0x1000, data: new Uint8Array(0x2000) }]) {
    const plan = planInstall({ parts: parts(), boardTable: ourTable(), flashBytes: 16 << 20, storeWrites: [w], storeWritable: true });
    assert.equal(plan.ok, false, `allowed a store write at 0x${w.offset.toString(16)}`);
  }
});
