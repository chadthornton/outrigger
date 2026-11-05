// Adding a Firmware Hub firmware from the page (site/installer/add.js): the
// catalog filter, the download check, and the link to the board, run against
// fm1_loader's own Receiver (build/receive_pipe), so both ends of the protocol
// are the code that ships.
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync, mkdtempSync, rmSync, readdirSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { spawn } from "node:child_process";
import { createHash } from "node:crypto";
import { addableFirmwares, fetchHubPackage, fromMutant, loadCatalog, BoardLink, encodeFrame, FrameReader, WRITE_PIECE }
  from "../../site/installer/add.js";

const root = new URL("../../", import.meta.url);
const sha256 = (b) => createHash("sha256").update(b).digest("hex");
const fixture = (name) => new Uint8Array(readFileSync(new URL(`tests/fixtures/${name}`, root)));
const PIPE = new URL("build/receive_pipe", root).pathname;
const ROOM = String(8 << 20);

// The board: receive_pipe over a fresh store directory, behind the same
// {write, read, close} the page builds from a Web Serial port.
function board({ free = ROOM, junk = false, dir = mkdtempSync(join(tmpdir(), "or-add-")) } = {}) {
  const child = spawn(PIPE, [dir, free, ...(junk ? ["junk"] : [])]);
  const chunks = [];
  let wake = null;
  const writes = [];
  child.stdout.on("data", (d) => { chunks.push(new Uint8Array(d)); if (wake) wake(); });
  return {
    dir,
    writes,
    async write(bytes) { writes.push(bytes.length); child.stdin.write(bytes); },
    async read() {
      while (!chunks.length) await new Promise((r) => { wake = r; });
      return chunks.shift();
    },
    close() { child.stdin.end(); child.kill(); rmSync(dir, { recursive: true, force: true }); },
  };
}

const entry = (name, version, project, file) => ({
  project, name, version, sha256: sha256(file), size: file.length, url: `https://thefirmwarehub.com/fw/${project}-${version}.fwsc`,
});

test("frames: what the page encodes, the page's reader decodes, past junk", () => {
  const f = encodeFrame(0x41, Uint8Array.of(1, 2, 3));
  assert.deepEqual([...f.subarray(0, 5)], [0x4f, 0x52, 0x41, 3, 0]);
  const r = new FrameReader();
  const got = r.push(Uint8Array.from([...new TextEncoder().encode("I (12) x: OR\n"), ...f]));
  assert.equal(got.length, 1);
  assert.equal(got[0].type, 0x41);
  assert.deepEqual([...got[0].payload], [1, 2, 3]);
});

test("hello and list on an empty store", async () => {
  const b = board();
  try {
    const link = new BoardLink(b);
    const h = await link.hello();
    assert.equal(h.protocol, 1);
    assert.equal(h.packages, 0);
    assert.equal(h.maxPackages, 10);
    assert.equal(h.free, 8 << 20);
    assert.equal(h.build, "test 1234567");
    assert.deepEqual(await link.list(), []);
  } finally { b.close(); }
});

test("add: a Hub package lands in the board's store, in pieces of at most 1 KiB", async () => {
  const b = board({ junk: true });
  try {
    const link = new BoardLink(b);
    const file = fixture("skiff-2.1.1.fwsc");
    const seen = [];
    const r = await link.add(entry("Skiff", "2.1.1", "skiff", file), file, (got, total) => seen.push([got, total]));
    assert.equal(r, "added");
    assert.deepEqual(seen.at(-1), [file.length, file.length]);
    assert.ok(Math.max(...b.writes) <= WRITE_PIECE);
    const list = await link.list();
    assert.deepEqual(list, [{ id: "skiff-2.1.1", sha256: sha256(file) }]);
    assert.ok(readdirSync(join(b.dir, "m", "pkg")).includes("skiff-2.1.1"));
    // Sent again: nothing to send, it's there.
    assert.equal(await link.add(entry("Skiff", "2.1.1", "skiff", file), file), "already");
  } finally { b.close(); }
});

test("add: a reply that comes late is used, never taken for the next request's", async () => {
  const b = board();
  try {
    // Some of the board's output arrives after the page has given up and resent.
    let n = 0;
    const read = b.read.bind(b);
    b.read = async () => {
      const bytes = await read();
      if (++n % 40 === 5) await new Promise((r) => setTimeout(r, 150));
      return bytes;
    };
    const link = new BoardLink(b, { replyMs: 100 });
    const file = fixture("felucca-1.8.0.fwsc");
    assert.equal(await link.add(entry("Felucca", "1.8.0", "felucca", file), file), "added");
    assert.deepEqual(await link.list(), [{ id: "felucca-1.8.0", sha256: sha256(file) }]);
  } finally { b.close(); }
});

test("add: a board that never moves on is given up on, not sent to forever", async () => {
  const b = board();
  try {
    const link = new BoardLink(b, { replyMs: 100 });
    const file = fixture("felucca-1.8.0.fwsc");
    // Larger pieces than the board takes: it acks the same offset each time.
    link.dataMax = 4000;
    await assert.rejects(link.add(entry("Felucca", "1.8.0", "felucca", file), file), /didn't answer/);
  } finally { b.close(); }
});

test("add: the board's refusal reaches the page in words", async () => {
  const b = board({ free: "100000" });
  try {
    const link = new BoardLink(b);
    const file = fixture("skiff-2.1.1.fwsc");
    await assert.rejects(link.add(entry("Skiff", "2.1.1", "skiff", file), file), /too full/);
  } finally { b.close(); }
});

const catalog = {
  stock: { pkg: "fw/mvave/FM-1_015.fwsc", sha256: "aa", size: 699956, product: "FM-1_015" },
  firmwares: [
    { id: "felucca", name: "Felucca", version: "1.8.0", pkg: "fw/felucca-1.8.0.fwsc", sha256: "bb", size: 610286, lic: "GPL-3.0", installs: 2610, base: [] },
    { id: "zp12", name: "zp12", version: "2.5", pkg: "fw/zp12-2.5.fwsc", sha256: "cc", size: 612200, lic: "GPL-3.0", installs: 325, base: [] },
    { id: "melodee", name: "Melodee", version: "1.6.0-rc.7", pkg: "fw/melodee.fwsc", sha256: "dd", size: 1, lic: "GPL-3.0", installs: 1829, base: [] },
    { id: "sloop", name: "SLOOP", version: "2.8.4", pkg: "fw/sloop-2.8.4.fwsc", sha256: "ee", size: 1, lic: "GPL-3.0", installs: 6854, base: [] },
    { id: "newsloop", name: "New on SLOOP", version: "0.1", pkg: "fw/newsloop-0.1.fwsc", sha256: "ff", size: 1, lic: "GPL-3.0", installs: 1, base: ["sloop"] },
    { id: "sloopdx", name: "DX SLOOP", version: "4.0.1", pkg: "fw/sloopdx-4.0.1.fwsc", sha256: "dx", size: 1, lic: "GPL-3.0", installs: 416, base: ["sloop"] },
    { id: "emb", name: "emb", version: "1", pkg: "fw/emb-1.fwsc", sha256: "11", size: 1, lic: null, installs: 5, base: [] },
    { id: "amenity", name: "Amenity", version: "1.0.0", pkg: "fw/amenity-1.0.0.fwsc", sha256: "22", size: 1, lic: "GPL-3.0", installs: 9, base: [] },
    { id: "nopkg", name: "No file", version: "1", pkg: null, sha256: null, size: 0, lic: "MIT", installs: 1, base: [] },
  ],
};

test("addableFirmwares: stock and the Hub list, most installed first, without the ones Outrigger can't take", () => {
  const { addable, held } = addableFirmwares(catalog);
  // sloopdx is built on SLOOP, but our survey cleared it by id (CLEARED): the base doesn't hold it.
  assert.deepEqual(addable.map((f) => f.project), ["felucca", "sloopdx", "zp12", "stock"]);
  assert.deepEqual(addable[0], { project: "felucca", name: "Felucca", version: "1.8.0", sha256: "bb", size: 610286,
    url: "https://thefirmwarehub.com/fw/felucca-1.8.0.fwsc", installs: 2610, family: "", lic: "GPL-3.0" });
  assert.equal(addable[3].name, "M-VAVE stock");
  assert.equal(addable[3].version, "V15");
  const why = Object.fromEntries(held.map((h) => [h.project, h.why]));
  assert.match(why.melodee, /Bluetooth/);
  assert.match(why.sloop, /older Felucca/);
  assert.match(why.newsloop, /older Felucca/);
  assert.match(why.emb, /licen[cs]e/);
  assert.match(why.amenity, /checks/);
  assert.equal(why.nopkg, undefined);  // no file to add: not listed at all
});

// mutant.cloud's two files, cut down to what the page reads.
const mutantCatalog = {
  stock: { pkg: "https://raw.githubusercontent.com/m-vave/fm1_open/b178/sdk/firmware/FM-1_015.fwsc", sha256: "aa", size: 699956, product: "FM-1_015" },
  firmwares: [
    { id: "felucca", entry: "felucca", name: "Felucca", version: "1.8.2-beta", file: "felucca-1.8.2-beta.fwsc",
      pkg: "https://github.com/hugelton/Felucca/releases/download/v1.8.2-beta/felucca-1.8.2-beta.fwsc", product: "FM-1_918", sha256: "bb", size: 610286, lic: "GPL-3.0" },
    { id: "skiff-fm-1-722", entry: "skiff", name: "Skiff", version: "2.2", file: "skiff-2.2.fwsc", pkg: "x", product: "FM-1_722", sha256: "cc", size: 1, lic: "GPL-3.0" },
    { id: "skiff-smk", entry: "skiff", name: "Skiff", version: "2.2", file: "skiff-2.2-smk37-elite.fwsc", pkg: "x", product: "SMK-37 Elite_722", sha256: "c2", size: 1, lic: "GPL-3.0" },
    { id: "choralroot", entry: "choralroot", name: "Choralroot", version: "0.29.2", file: "choralroot-0.29.2-1core.fwsc", pkg: "x", product: "FM-1_920", sha256: "d1", size: 1, lic: "GPL-3.0" },
    { id: "choralroot-fm-1-920", entry: "choralroot", name: "Choralroot", version: "0.29.2", file: "choralroot-0.29.2.fwsc", pkg: "x", product: "FM-1_920", sha256: "d2", size: 1, lic: "GPL-3.0" },
    { id: "fm1-dock", entry: "fm1-dock", name: "fm1-dock", version: "1.0", file: "fm1-dock-1.0.fwsc", pkg: "x", product: "FM-1_960", sha256: "ee", size: 1, lic: "MIT" },
    { id: "orange-cat", entry: "orange-cat", name: "Orange Cat", version: "1", file: "orange-cat-1.fwsc", pkg: "x", product: "FM-1_970", sha256: "ff", size: 1, lic: "GPL-3.0" },
    { id: "melodee-bt", entry: "melodee", name: "Melodee", version: "1.6", file: "melodee-bt.fwsc", pkg: "x", product: "FM-1_91061", sha256: "m1", size: 1, lic: "GPL-3.0" },
    { id: "melodee", entry: "melodee", name: "Melodee", version: "1.6", file: "melodee.fwsc", pkg: "x", product: "FM-1_91061", sha256: "m2", size: 1, lic: "GPL-3.0" },
    { id: "mycelium", entry: "mycelium", name: "Mycelium", version: "1", file: "mycelium-1.fwsc", pkg: "x", product: "FM-1_918", sha256: "my", size: 1, lic: "GPL-3.0" },
    { id: "clipper", entry: "clipper", name: "Clipper", version: "1", file: "clipper-1.fwsc", pkg: "x", product: "FM-1_900", sha256: "11", size: 1, lic: "GPL-3.0" },
  ],
};
const mutantFamilies = {
  bases: { felucca: { name: "Felucca" }, sloop: { name: "SLOOP" }, melodee: { name: "Melodee" }, original: { name: "its own code" } },
  entries: [
    { id: "felucca", base: null }, { id: "skiff", base: "felucca" }, { id: "choralroot", base: "original" },
    { id: "orange-cat", base: "melodee" }, { id: "mycelium", base: "felucca" }, { id: "melodee", base: "felucca" }, { id: "clipper", base: "sloop" },
  ],
};

test("fromMutant: FM-1 files only, through mutant.cloud's /dl, with the Hub's ids and each file its own shelf", () => {
  const { addable, held } = addableFirmwares(fromMutant(mutantCatalog, mutantFamilies));
  const by = Object.fromEntries(addable.map((f) => [f.project, f]));
  // Choralroot's main file is the one named choralroot-0.29.2.fwsc (the Hub's), though its row id isn't
  // the entry; the one-core build, whose row id is the entry, gets a project of its own.
  assert.equal(addable.length, Object.keys(by).length);  // no two files share a project
  assert.deepEqual(Object.keys(by).sort(), ["choralroot", "choralroot-choralroot-0.29.2-1core", "felucca", "fm1dock", "skiff", "stock"]);
  assert.equal(by.choralroot.url, "https://mutant.cloud/dl/choralroot/choralroot-0.29.2.fwsc");
  // The entry is the project, as boards' shelves and the loader's tables know it ("skiff", not "skiff-fm-1-722").
  assert.equal(by.skiff.url, "https://mutant.cloud/dl/skiff/skiff-2.2.fwsc");
  assert.equal(by.skiff.family, "Felucca");
  assert.equal(by.skiff.lic, "GPL-3.0");
  assert.equal(by.felucca.family, "");
  assert.equal(by.choralroot.family, "");  // "its own code" isn't a family
  assert.match(by["choralroot-choralroot-0.29.2-1core"].name, /\(choralroot-0\.29\.2-1core\)/);
  assert.equal(by.fm1dock.url, "https://mutant.cloud/dl/fm1-dock/fm1-dock-1.0.fwsc");
  assert.equal(by.stock.url, mutantCatalog.stock.pkg);
  assert.equal(by.stock.version, "V15");
  const why = Object.fromEntries(held.map((h) => [h.project, h.why]));
  assert.match(why["orange-cat"], /Bluetooth/);  // blocked by its own id in the generated list
  assert.match(why.clipper, /older Felucca/);
  assert.match(why.mycelium, /older Felucca/);  // the Hub says built on SLOOP too
  // Melodee is listed as built on Felucca: its id holds it, and its other file too.
  assert.match(why.melodee, /Bluetooth/);
  assert.match(why["melodee-bt"], /Bluetooth/);
});

test("fromMutant: without firmware.json the list still works, families blank", () => {
  const { addable } = addableFirmwares(fromMutant(mutantCatalog, null));
  assert.ok(addable.every((f) => f.family === ""));
  assert.ok(addable.some((f) => f.project === "skiff"));
});

// A fetch that answers per URL: a value is its JSON, a number its status, "hang" never answers in time.
const site = (answers) => async (url, opts = {}) => {
  const key = Object.keys(answers).find((k) => url.startsWith(k));
  const a = key ? answers[key] : 404;
  if (a === "hang") {
    return new Promise((_, reject) => opts.signal.addEventListener("abort", () => reject(opts.signal.reason)));
  }
  if (a instanceof Error) throw a;
  return typeof a === "number" ? { ok: false, status: a } : { ok: true, status: 200, json: async () => a };
};
const HUB_CAT = "https://thefirmwarehub.com/catalog.json";

test("loadCatalog: mutant.cloud first", async () => {
  const c = await loadCatalog(site({ "https://mutant.cloud/catalog.json": mutantCatalog, "https://mutant.cloud/firmware.json": mutantFamilies, [HUB_CAT]: catalog }));
  assert.equal(c.from, "mutant.cloud");
  assert.ok(c.firmwares.some((f) => f.family === "Felucca"));
});

test("loadCatalog: the Hub when mutant.cloud fails, is slow or sends nothing", async () => {
  for (const down of [503, new TypeError("Failed to fetch"), "hang", { firmwares: [] }]) {
    const c = await loadCatalog(site({ "https://mutant.cloud/": down, [HUB_CAT]: catalog }), 50);
    assert.equal(c.from, "the Firmware Hub");
    const { addable } = addableFirmwares(c);
    assert.equal(addable[0].url, "https://thefirmwarehub.com/fw/felucca-1.8.0.fwsc");
  }
});

test("loadCatalog: both down says so", async () => {
  await assert.rejects(loadCatalog(site({ "https://mutant.cloud/": 503, [HUB_CAT]: 500 }), 50), /mutant\.cloud.*Firmware Hub answered 500/);
});

test("fetchHubPackage: messages name where the file comes from", async () => {
  const file = fixture("felucca-1.8.0.fwsc");
  const fw = entry("Felucca", "1.8.0", "felucca", file);
  await assert.rejects(fetchHubPackage(fw, answer(file, 502), "mutant.cloud"), /^Error: mutant\.cloud answered 502/);
});

const answer = (bytes, status = 200) => async () => ({ ok: status === 200, status, arrayBuffer: async () => bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.length) });

test("fetchHubPackage: the file the catalog names, checked and parsed", async () => {
  const file = fixture("felucca-1.8.0.fwsc");
  const fw = entry("Felucca", "1.8.0", "felucca", file);
  assert.deepEqual(await fetchHubPackage(fw, answer(file)), file);
  const damaged = Uint8Array.from(file);
  damaged[100] ^= 1;
  await assert.rejects(fetchHubPackage(fw, answer(damaged)), /didn't match/);
  await assert.rejects(fetchHubPackage(fw, answer(file, 404)), /answered 404/);
  const junk = new Uint8Array(4000).fill(7);
  await assert.rejects(fetchHubPackage({ ...fw, sha256: sha256(junk) }, answer(junk)), /isn't an FM-1 firmware Outrigger can use/);
});
