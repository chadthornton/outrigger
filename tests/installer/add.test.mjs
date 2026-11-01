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
import { addableFirmwares, fetchHubPackage, BoardLink, encodeFrame, FrameReader, WRITE_PIECE }
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
    { id: "emb", name: "emb", version: "1", pkg: "fw/emb-1.fwsc", sha256: "11", size: 1, lic: null, installs: 5, base: [] },
    { id: "amenity", name: "Amenity", version: "1.0.0", pkg: "fw/amenity-1.0.0.fwsc", sha256: "22", size: 1, lic: "GPL-3.0", installs: 9, base: [] },
    { id: "nopkg", name: "No file", version: "1", pkg: null, sha256: null, size: 0, lic: "MIT", installs: 1, base: [] },
  ],
};

test("addableFirmwares: stock and the Hub list, most installed first, without the ones Outrigger can't take", () => {
  const { addable, held } = addableFirmwares(catalog);
  assert.deepEqual(addable.map((f) => f.project), ["felucca", "zp12", "stock"]);
  assert.deepEqual(addable[0], { project: "felucca", name: "Felucca", version: "1.8.0", sha256: "bb", size: 610286,
    url: "https://thefirmwarehub.com/fw/felucca-1.8.0.fwsc", installs: 2610 });
  assert.equal(addable[2].name, "M-VAVE stock");
  assert.equal(addable[2].version, "V15");
  const why = Object.fromEntries(held.map((h) => [h.project, h.why]));
  assert.match(why.melodee, /Bluetooth/);
  assert.match(why.sloop, /older Felucca/);
  assert.match(why.newsloop, /older Felucca/);
  assert.match(why.emb, /licen[cs]e/);
  assert.match(why.amenity, /checks/);
  assert.equal(why.nopkg, undefined);  // no file to add: not listed at all
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
