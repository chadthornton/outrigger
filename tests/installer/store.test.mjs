// Setting up the store on a board that has none (site/installer/store.js),
// and the planner's one permission to write it (plan.js).
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync, existsSync } from "node:fs";
import { createHash } from "node:crypto";
import { storeAction, fetchLoader, fetchStarter, buildStore, parsePackage } from "../../site/installer/store.js";
import { planInstall } from "../../site/installer/plan.js";

const root = new URL("../../", import.meta.url);
const sha256 = (b) => createHash("sha256").update(b).digest("hex");
const md5 = (b) => createHash("md5").update(b).digest("hex");

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
  assert.equal(storeAction({ runsLoader: true, hasStore: false, hadStorePartition: true, blank: false, unfinished: false }), "unreadable");
});

// Chad, 2026-11-01: an install cut short mid-store (a pulled cable) is finished
// by the next one, when the region holds nothing but this installer's own writes.
test("storeAction: a store this installer started and didn't finish is finished", () => {
  assert.equal(storeAction({ runsLoader: true, hasStore: false, hadStorePartition: true, blank: false, unfinished: true }), "create");
});

// Chad, 2026-10-31: a board that ran other firmware (no store in its table)
// has that firmware's leftovers where the store goes, not FM-1 backups.
test("storeAction: a board from other firmware gets a store over its leftovers", () => {
  assert.equal(storeAction({ runsLoader: true, hasStore: false, hadStorePartition: false, blank: false }), "create");
  assert.equal(storeAction({ runsLoader: true, hasStore: false, hadStorePartition: false, blank: true }), "create");
});

// The real template, and what tools/store_template.py checked by mounting it
// (tests/installer/test_store_template.py runs first in make test).
const templatePath = new URL("build/store-template.json", root);
const expectedPath = new URL("build/store-spliced.sha256", root);
const appsPath = new URL("build/starter-apps.json", root);
const fixture = (name) => new Uint8Array(readFileSync(new URL(`tests/fixtures/${name}`, root)));
const haveReal = existsSync(templatePath) && existsSync(expectedPath) && existsSync(appsPath);
const skip = !haveReal && "run test_store_template.py first";

test("parsePackage decodes every fixture package to the identity and app the Python port does", { skip }, () => {
  const apps = JSON.parse(readFileSync(appsPath));
  assert.ok(Object.keys(apps).length >= 6);
  for (const [name, [identity, appSha]] of Object.entries(apps)) {
    const got = parsePackage(fixture(name));
    assert.equal(got.identity, identity, name);
    assert.equal(sha256(got.app), appSha, name);
  }
});

test("parsePackage refuses what isn't an FM-1 package", { skip }, () => {
  assert.throws(() => parsePackage(new Uint8Array(100)), /too short/);
  const bad = fixture("skiff-2.1.1.fwsc");
  bad[5] ^= 0xff; // inside the UFW header
  assert.throws(() => parsePackage(bad), /CRC/);
  assert.throws(() => parsePackage(new Uint8Array(0x2000).fill(0xff)), /CRC|identity/);
});

// What the browser downloads: the loader and stock V15, each from a pinned commit on GitHub.
const realFiles = () => {
  const template = JSON.parse(readFileSync(templatePath));
  const files = new Map([[template.loader.url, fixture("wl82loader.bin")]]);
  for (const p of template.packages) files.set(p.url, fixture(p.url.split("/").pop()));
  return { template, files };
};
const serving = (files, override = {}) => async (url, opts) => {
  if (url in override) return override[url](opts);
  const data = files.get(url);
  return data ? { ok: true, arrayBuffer: async () => data.slice().buffer } : { ok: false, status: 404 };
};

test("buildStore rebuilds the store the Mac mounted, run by run, the superblock pair last", { skip }, async () => {
  const { template, files } = realFiles();
  const got = await fetchStarter(template, serving(files));
  const runs = await buildStore(template, got);
  assert.deepEqual(runs.map((r) => [r.offset, r.data.length, r.md5]), template.runs.map((r) => [r.offset, r.size, r.md5]));
  assert.equal(runs.at(-1).offset, 0);
  assert.ok(runs.every((r) => md5(r.data) === r.md5));
  const blocks = runs.flatMap((r) => [...Array(r.data.length / 0x1000)].map((_, i) => [r.offset + i * 0x1000, r.data.subarray(i * 0x1000, (i + 1) * 0x1000)]))
    .sort((a, b) => a[0] - b[0]);
  const joined = Buffer.concat(blocks.flatMap(([off, data]) => [Buffer.from(new Uint32Array([off]).buffer), Buffer.from(data)]));
  assert.equal(sha256(joined), readFileSync(expectedPath, "utf8").trim());
});

test("buildStore refuses a store that isn't the one the template describes", { skip }, async () => {
  const { template, files } = realFiles();
  const got = await fetchStarter(template, serving(files));
  const older = { loader: got.loader, apps: new Map() };
  await assert.rejects(buildStore(template, older), /missing/);
  const loader = got.loader.slice();
  loader[100] ^= 0xff;
  await assert.rejects(buildStore(template, { loader, apps: got.apps }), /didn't match/);
  const i = template.blocks.findIndex((x) => x.hex && x.offset > 0x1000);
  const flip = (h) => (parseInt(h.slice(0, 2), 16) ^ 0xff).toString(16).padStart(2, "0") + h.slice(2);
  const tampered = { ...template, blocks: template.blocks.map((x, j) => (j === i ? { ...x, hex: flip(x.hex) } : x)) };
  await assert.rejects(buildStore(tampered, got), /didn't match/);
});

test("fetchStarter names the firmware that didn't download, and why", { skip }, async () => {
  const { template, files } = realFiles();
  const felucca = template.packages.find((p) => p.id === "stock-V15").url;
  await assert.rejects(fetchStarter(template, serving(files, { [felucca]: async () => ({ ok: false, status: 404 }) })), /404 for M-VAVE stock V15/);
  await assert.rejects(fetchStarter(template, serving(files, { [felucca]: async () => { throw new TypeError("Failed to fetch"); } })),
    /couldn't reach GitHub.*M-VAVE stock V15/i);
  const wrong = fixture("felucca-1.6.1.fwsc");
  await assert.rejects(fetchStarter(template, serving(files, { [felucca]: async () => ({ ok: true, arrayBuffer: async () => wrong.buffer }) })),
    /M-VAVE stock V15.*didn't match/);
  await assert.rejects(fetchStarter(template, serving(files, { [template.loader.url]: async () => ({ ok: false, status: 503 }) })), /GitHub answered 503/);
});

test("fetchStarter gives every download a deadline", { skip }, async () => {
  const { template, files } = realFiles();
  const signals = [];
  await fetchStarter(template, async (url, opts) => { signals.push(opts && opts.signal); return serving(files)(url); });
  assert.equal(signals.length, 1 + template.packages.length);
  assert.ok(signals.every((s) => s instanceof AbortSignal));
});

const tiny = {
  size: 0x3000,
  blob_size: 8,
  loader: { url: "https://example.invalid/wl82loader.bin", sha256: sha256(Buffer.from("LOADERAB")) },
  packages: [],
};
const ok = (bytes) => async () => ({ ok: true, arrayBuffer: async () => Uint8Array.from(bytes).buffer });

test("fetchLoader returns the loader when its SHA-256 matches", async () => {
  const got = await fetchLoader(tiny, ok(Buffer.from("LOADERAB")));
  assert.equal(new TextDecoder().decode(got), "LOADERAB");
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
    storeWrites: [storeBlock(0x2000), storeBlock(0)], storeWritable: true });
  assert.deepEqual(plan.problems, []);
  assert.deepEqual(plan.writes.map((w) => w.offset), [0x0, 0x8000, 0xe000, 0x10000, 0x3d2000, 0x3d0000]);
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
