// The install engine (site/installer/install.js) against a fake board: 16 MB
// of flash in memory behind the few esptool-js calls the engine makes.
import { test } from "node:test";
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { readBoard, install, restart, finish, fetchBuild, storeIsBlank, prepareStore } from "../../site/installer/install.js";

const MB = 1024 * 1024;
const md5 = (b) => createHash("md5").update(b).digest("hex");
const sha256 = (b) => createHash("sha256").update(b).digest("hex");

// Our partition table, as the chip stores it.
const OURS = [["nvs", 1, 0x9000, 0x5000], ["otadata", 1, 0xe000, 0x2000], ["app0", 0, 0x10000, 0x300000],
  ["recovery", 0, 0x310000, 0xc0000], ["store", 1, 0x3d0000, 0xc20000], ["coredump", 1, 0xff0000, 0x10000]];
function table(entries = OURS) {
  const out = new Uint8Array(0xc00).fill(0xff);
  const view = new DataView(out.buffer);
  entries.forEach(([name, type, off, size], i) => {
    out.fill(0, i * 32, i * 32 + 32);
    out.set([0xaa, 0x50, type, 0], i * 32);
    view.setUint32(i * 32 + 4, off, true);
    view.setUint32(i * 32 + 8, size, true);
    out.set(new TextEncoder().encode(name), i * 32 + 12);
  });
  out.set([0xeb, 0xeb], entries.length * 32);
  return out;
}
function app(commit, label, length = 900000) {
  const out = new Uint8Array(length).fill(0x5a);
  out[0] = 0xe9;
  const view = new DataView(out.buffer);
  view.setUint32(0x120, 0x54484d55, true);
  view.setUint16(0x124, 1, true);
  view.setUint16(0x126, 0, true); // clean, reserved
  view.setUint32(0x128, commit, true);
  out.set(new TextEncoder().encode(label), 0x12c);
  out.fill(0, 0x12c + label.length, 0x134);
  return out;
}

class FakeBoard {
  constructor() {
    this.flash = new Uint8Array(16 * MB).fill(0xff);
    this.calls = [];
    this.corrupt = null; // an offset whose MD5 comes back wrong
  }
  async readFlash(addr, size, onPacket) {
    this.calls.push(["readFlash", addr, size]);
    const out = this.flash.slice(addr, addr + size);
    if (onPacket) onPacket(out, size, size);
    return out;
  }
  async writeFlash(options) {
    this.calls.push(["writeFlash", options]);
    if (options.eraseAll) this.flash.fill(0xff);
    options.fileArray.forEach(({ data, address }, i) => {
      this.flash.fill(0xff, address, address + Math.ceil(data.length / 0x1000) * 0x1000);
      this.flash.set(data, address);
      options.reportProgress?.(i, data.length, data.length);
    });
  }
  async flashMd5sum(addr, size) {
    this.calls.push(["flashMd5sum", addr, size]);
    return addr === this.corrupt ? "0".repeat(32) : md5(this.flash.subarray(addr, addr + size));
  }
  async writeReg(addr, value) {
    this.calls.push(["writeReg", addr, value]);
  }
}

function boardWithLoaderAndBackups() {
  const b = new FakeBoard();
  b.flash.set(table(), 0x8000);
  b.flash.set(app(0xabc1234, "hub+L"), 0x10000);
  b.flash.fill(0x11, 0x9000, 0xe000); // settings
  b.flash.fill(0x22, 0x3d0000, 0x3d0000 + 0xc20000); // the store
  b.flash.set(new TextEncoder().encode("littlefs"), 0x3d0000 + 8);
  return b;
}

function hubBuild() {
  const parts = [
    { offset: 0x0, data: Object.assign(new Uint8Array(20000).fill(1), { 0: 0xe9 }) },
    { offset: 0x8000, data: table() },
    { offset: 0xe000, data: new Uint8Array(0x2000).fill(3) },
    { offset: 0x10000, data: app(0xdef5678, "hub", 1000000) },
  ];
  const manifest = {
    version: "def5678",
    build: { commit: "def5678", dirty: false, app_set: "hub" },
    builds: [{ chipFamily: "ESP32-S3", parts: parts.map(({ offset, data }) => ({ path: `p${offset}.bin`, offset, md5: md5(data), sha256: sha256(data) })) }],
  };
  return { manifest, parts };
}

test("readBoard: a blank board", async () => {
  const info = await readBoard(new FakeBoard());
  assert.equal(info.layout, "blank");
  assert.equal(info.tag, null);
  assert.equal(info.store, false);
});

test("readBoard: Hub + Loader with a store, without reading the store itself", async () => {
  const b = boardWithLoaderAndBackups();
  const info = await readBoard(b);
  assert.equal(info.layout, "ours");
  assert.deepEqual(info.tag, { commit: "abc1234", dirty: false, app_set: "hub+L" });
  assert.equal(info.store, true);
  assert.deepEqual(info.storeRange, { offset: 0x3d0000, size: 0xc20000 });
  const read = b.calls.filter((c) => c[0] === "readFlash").reduce((n, c) => n + c[2], 0);
  assert.ok(read <= 4096, `read ${read} bytes before installing`);
});

test("install: writes the four parts, keeps flash settings, verifies each, leaves settings and the store alone", async () => {
  const b = boardWithLoaderAndBackups();
  const before = b.flash.slice();
  const { manifest, parts } = hubBuild();
  const steps = [];
  await install({ loader: b, board: await readBoard(b), manifest, parts, onStep: (s) => steps.push(s.step) });

  const writes = b.calls.filter((c) => c[0] === "writeFlash");
  assert.equal(writes.length, 1);
  const opts = writes[0][1];
  assert.equal(opts.eraseAll, false);
  assert.deepEqual([opts.flashMode, opts.flashFreq, opts.flashSize], ["keep", "keep", "keep"]);
  assert.deepEqual(opts.fileArray.map((f) => f.address), [0x0, 0x8000, 0xe000, 0x10000]);
  assert.deepEqual(b.calls.filter((c) => c[0] === "flashMd5sum").map((c) => c[1]), [0x0, 0x8000, 0xe000, 0x10000]);
  assert.deepEqual(b.flash.subarray(0x9000, 0xe000), before.subarray(0x9000, 0xe000), "settings changed");
  assert.deepEqual(b.flash.subarray(0x310000), before.subarray(0x310000), "recovery, store or coredump changed");
  assert.ok(steps.includes("verify"));
});

test("install: refuses before writing when the build would move the board's store", async () => {
  const b = boardWithLoaderAndBackups();
  const { manifest, parts } = hubBuild();
  const moved = OURS.map((e) => (e[0] === "store" ? ["store", 1, 0x3e0000, 0xc10000] : e));
  parts[1] = { offset: 0x8000, data: table(moved) };
  await assert.rejects(install({ loader: b, board: await readBoard(b), manifest, parts }), /backup store/);
  assert.equal(b.calls.filter((c) => c[0] === "writeFlash").length, 0);
});

test("install: a part that reads back wrong fails, naming the part", async () => {
  const b = boardWithLoaderAndBackups();
  b.corrupt = 0x10000;
  const { manifest, parts } = hubBuild();
  await assert.rejects(install({ loader: b, board: await readBoard(b), manifest, parts }), /program/);
});

test("restart: an RTC watchdog reset, which starts the program even after BOOT+RST", async () => {
  const b = new FakeBoard();
  await restart(b);
  assert.deepEqual(b.calls.map((c) => c.slice(1)), [
    [0x600080b0, 0x50d83aa1], // unlock
    [0x6000809c, 2000],       // timeout
    [0x60008098, (1 << 31) | (5 << 28) | (1 << 8) | 2 >>> 0], // enable: stage 0 resets the system
    [0x600080b0, 0],          // lock
  ].map(([a, v]) => [a, v >>> 0]));
});

test("fetchBuild: refuses a part whose bytes don't match the manifest", async () => {
  const { manifest, parts } = hubBuild();
  const files = new Map(parts.map((p) => [`p${p.offset}.bin`, p.data]));
  files.set("p65536.bin", new Uint8Array(10).fill(9));
  const fetch = async (url) => {
    const name = url.split("/").pop();
    if (name === "manifest.json") return { ok: true, json: async () => manifest };
    return { ok: true, arrayBuffer: async () => files.get(name).slice().buffer };
  };
  await assert.rejects(fetchBuild("fw/hub/manifest.json", fetch), /program/);
});

test("fetchBuild: returns the four parts in offset order", async () => {
  const { manifest, parts } = hubBuild();
  const files = new Map(parts.map((p) => [`p${p.offset}.bin`, p.data]));
  const fetch = async (url) => {
    const name = url.split("/").pop();
    if (name === "manifest.json") return { ok: true, json: async () => manifest };
    return { ok: true, arrayBuffer: async () => files.get(name).slice().buffer };
  };
  const got = await fetchBuild("fw/hub/manifest.json", fetch);
  assert.deepEqual(got.parts.map((p) => p.offset), [0x0, 0x8000, 0xe000, 0x10000]);
  assert.deepEqual(got.manifest.build, manifest.build);
});

test("finish: a board that resets mid-restart and a port that never closes still finish", async () => {
  const b = new FakeBoard();
  let n = 0;
  b.writeReg = async (addr, value) => {
    b.calls.push(["writeReg", addr, value]);
    if (++n > 3) throw new Error("port gone"); // the chip resets before the lock write lands
  };
  const transport = { disconnect: () => new Promise(() => {}) }; // hangs
  const t0 = Date.now();
  await finish({ loader: b, transport, timeoutMs: 50 });
  assert.ok(Date.now() - t0 < 1000);
  assert.equal(b.calls.filter((c) => c[0] === "writeReg").length, 4);
});

test("finish: a restart that fails before the watchdog is armed still finishes", async () => {
  const b = new FakeBoard();
  b.writeReg = async () => { throw new Error("no stub"); };
  await finish({ loader: b, transport: { disconnect: async () => {} }, timeoutMs: 50 });
});

// A board with our table and a factory-blank store region: what a Hub + Loader
// install from the page finds on a new Waveshare.
function boardWithBlankStore() {
  const b = new FakeBoard();
  b.flash.set(table(), 0x8000);
  b.flash.set(app(0xabc1234, "hub"), 0x10000);
  return b;
}
const BLANK_MD5 = md5(new Uint8Array(0xc20000).fill(0xff));
const RANGE = { offset: 0x3d0000, size: 0xc20000 };

test("storeIsBlank: the board's MD5 of the store region against an erased region's", async () => {
  assert.equal(await storeIsBlank(boardWithBlankStore(), RANGE, BLANK_MD5), true);
  const b = boardWithBlankStore();
  b.flash[0x3d0000 + 0x800000] = 0x00; // one byte of old data, deep in the region
  assert.equal(await storeIsBlank(b, RANGE, BLANK_MD5), false);
});

test("install: writes a store onto a blank region and reads it back", async () => {
  const b = boardWithBlankStore();
  const { manifest, parts } = hubBuild();
  const store = [{ offset: 0x3d0000, data: new Uint8Array(0x1000).fill(0x42) }, { offset: 0x3d4000, data: new Uint8Array(0x1000).fill(0x43) }];
  const steps = [];
  await install({ loader: b, board: await readBoard(b), manifest, parts, storeWrites: store, storeBlank: true, onStep: (s) => steps.push(s.step) });
  assert.deepEqual(b.flash.subarray(0x3d0000, 0x3d1000), store[0].data);
  assert.deepEqual(b.flash.subarray(0x3d4000, 0x3d5000), store[1].data);
  assert.ok(steps.includes("verify store"));
  const opts = b.calls.find((c) => c[0] === "writeFlash")[1];
  assert.deepEqual(opts.fileArray.map((f) => f.address), [0x0, 0x8000, 0xe000, 0x10000, 0x3d0000, 0x3d4000]);
});

test("install: a store block that reads back wrong is reported, after a verified program install", async () => {
  const b = boardWithBlankStore();
  const realWrite = b.writeFlash.bind(b);
  b.writeFlash = async (o) => { await realWrite(o); b.flash[0x3d0010] ^= 0xff; };
  const { manifest, parts } = hubBuild();
  const result = await install({ loader: b, board: await readBoard(b), manifest, parts,
    storeWrites: [{ offset: 0x3d0000, data: new Uint8Array(0x1000).fill(0x42) }], storeBlank: true });
  assert.match(result.storeProblem, /store/);
});

// prepareStore: what the install does to the store, worked out before anything is written.
const TEMPLATE = {
  size: 0xc20000, blank_md5: BLANK_MD5, blob_size: 8,
  loader: { url: "https://example.invalid/wl82loader.bin", sha256: createHash("sha256").update("LOADERAB").digest("hex") },
  blocks: [{ offset: 0x0, hex: "00".repeat(0x1000) }, { offset: 0x2000, hex: "11".repeat(0x1000) }],
  extents: [[0x2008, 0, 8]],
};
const loaderBuild = () => ({ ...hubBuild(), template: TEMPLATE });
const github = async () => ({ ok: true, arrayBuffer: async () => new TextEncoder().encode("LOADERAB").buffer });

test("prepareStore: MIDI Hub leaves the store alone, without asking the board", async () => {
  const b = boardWithBlankStore();
  const got = await prepareStore({ loader: b, board: await readBoard(b), build: hubBuild(), fetchFn: github });
  assert.equal(got.action, "none");
  assert.equal(b.calls.filter((c) => c[0] === "flashMd5sum").length, 0);
});

test("prepareStore: a blank region gets the template with the loader, at the store's offset", async () => {
  const b = boardWithBlankStore();
  const got = await prepareStore({ loader: b, board: await readBoard(b), build: loaderBuild(), fetchFn: github });
  assert.equal(got.action, "create");
  assert.equal(got.storeBlank, true);
  assert.deepEqual(got.storeWrites.map((w) => w.offset), [0x3d0000, 0x3d2000]);
  assert.equal(new TextDecoder().decode(got.storeWrites[1].data.subarray(8, 16)), "LOADERAB");
  assert.equal(got.loaderProblem, null);
});

test("prepareStore: data with no readable store stops before anything is written", async () => {
  const b = boardWithBlankStore();
  b.flash.fill(0x00, 0x3d0000, 0x3d1000); // something, but not a store
  const got = await prepareStore({ loader: b, board: await readBoard(b), build: loaderBuild(), fetchFn: github });
  assert.equal(got.action, "unreadable");
  assert.deepEqual(got.storeWrites, []);
});

test("prepareStore: an existing store is kept and the loader isn't downloaded", async () => {
  const b = boardWithLoaderAndBackups();
  let fetched = 0;
  const got = await prepareStore({ loader: b, board: await readBoard(b), build: loaderBuild(), fetchFn: async () => { fetched++; return github(); } });
  assert.equal(got.action, "keep");
  assert.equal(fetched, 0);
});

test("prepareStore: a template that doesn't fit the build's store is a build error, not unreadable storage", async () => {
  const b = boardWithBlankStore();
  const build = { ...hubBuild(), template: { ...TEMPLATE, size: 0x100000 } };
  await assert.rejects(prepareStore({ loader: b, board: await readBoard(b), build, fetchFn: github }), /template/);
});

test("prepareStore: if GitHub fails, the install goes ahead without a store and says why", async () => {
  const b = boardWithBlankStore();
  const got = await prepareStore({ loader: b, board: await readBoard(b), build: loaderBuild(), fetchFn: async () => ({ ok: false, status: 503 }) });
  assert.equal(got.action, "create");
  assert.deepEqual(got.storeWrites, []);
  assert.match(got.loaderProblem, /503/);
});

test("fetchBuild: a loader build's store template must match the manifest's SHA-256", async () => {
  const { manifest, parts } = hubBuild();
  const templateText = JSON.stringify(TEMPLATE);
  const files = new Map(parts.map((p) => [`p${p.offset}.bin`, p.data]));
  files.set("store-template.json", new TextEncoder().encode(templateText));
  const serve = (m) => async (url) => {
    const name = url.split("/").pop();
    if (name === "manifest.json") return { ok: true, json: async () => m };
    return { ok: true, arrayBuffer: async () => files.get(name).slice().buffer };
  };
  const good = { ...manifest, store_template: "store-template.json", store_template_sha256: createHash("sha256").update(templateText).digest("hex") };
  assert.deepEqual((await fetchBuild("fw/hub-loader/manifest.json", serve(good))).template, TEMPLATE);
  await assert.rejects(fetchBuild("fw/hub-loader/manifest.json", serve({ ...good, store_template_sha256: "0".repeat(64) })), /template/);
});
