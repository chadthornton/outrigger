// Adding a Firmware Hub firmware to an Outrigger that's set up: the Hub's
// list, minus the firmwares Outrigger can't take yet; the file downloaded
// into the visitor's browser and checked against the catalog's SHA-256; then
// sent over Web Serial to the board in Computer mode, which checks it again
// and stores it (lib/fm1_loader/src/receive.h has the protocol). This site
// never hosts a firmware. Pure, so `make test` runs it against the board's own
// receiver (tests/installer/add.test.mjs).
import { crc16, parsePackage, sha256Hex } from "./store.js";

export const HUB = "https://thefirmwarehub.com/";
export const CATALOG_URL = `${HUB}catalog.json`;

// Projects the board's loader blocks (library.cpp kBlocked): they write the
// FM-1's Bluetooth sector, whose hash is how Outrigger tells FM-1s apart.
const BLOCKED = ["melodee", "tessera"];
// Built on Felucca 0.9 (SLOOP and its family), from the 2026-11-01 survey of
// every Hub firmware's source (fw-map-bench.md). Going from one of these to
// Felucca or Skiff can leave data that crashes them (Chad, 2026-11-01: SLOOP
// waits until a store dump shows where it keeps its data). A firmware whose
// catalog "base" names one of these is held too. Firmwares added to the Hub
// after the survey aren't classified: they're listed, and switch with a clean
// slate like every untested switch.
const OLD_FELUCCA = ["sloop", "clipper", "spampy", "winampfm", "floyd", "jangada", "nmsfm", "karma", "sloopdx",
  "hortator", "sloopbanks", "fm1quest", "mnm1", "salt", "vector37", "nauclerus", "gator", "hawkchua"];

// Files that fail Outrigger's package checks (make check-latest LATEST_ALL=1,
// 2026-11-01: 1 of 143). Any other that fails is caught at download.
const FAILS_CHECKS = ["amenity"];

const WHY_BLOCKED = "Not yet: it writes the FM-1's Bluetooth sector, which Outrigger uses to tell FM-1s apart.";
const WHY_OLD = "Not yet: it's built on an older Felucca, and switching from it to Felucca or Skiff could crash them.";
const WHY_CHECKS = "Not yet: its file doesn't pass Outrigger's checks (its firmware is cut short).";
const WHY_LICENSE = "Not from the list: it has no license, so add it yourself once files of your own are allowed.";

// {addable, held}: addable are {project, name, version, sha256, size, url,
// installs}, stock last; held are {project, name, why}. Entries without a
// file aren't in either.
export function addableFirmwares(catalog) {
  const addable = [];
  const held = [];
  for (const e of catalog.firmwares || []) {
    if (!e.pkg || !e.sha256) continue;
    const base = e.base || [];
    const why = BLOCKED.includes(e.id) ? WHY_BLOCKED
      : OLD_FELUCCA.includes(e.id) || base.some((b) => OLD_FELUCCA.includes(b)) ? WHY_OLD
        : FAILS_CHECKS.includes(e.id) ? WHY_CHECKS : !e.lic ? WHY_LICENSE : null;
    if (why) held.push({ project: e.id, name: e.name, why });
    else addable.push({ project: e.id, name: e.name, version: e.version || "", sha256: e.sha256, size: e.size,
      url: HUB + e.pkg, installs: e.installs || 0 });
  }
  addable.sort((a, b) => b.installs - a.installs || a.name.localeCompare(b.name));
  const s = catalog.stock;
  if (s && s.pkg && s.sha256) {
    // As tools/store_add.py names it: "stock", version "V15".
    addable.push({ project: "stock", name: "M-VAVE stock", version: `V${parseInt(s.product.slice(5), 10)}`,
      sha256: s.sha256, size: s.size, url: HUB + s.pkg, installs: 0 });
  }
  return { addable, held };
}

const DOWNLOAD_TIMEOUT_MS = 30000;

// The file, checked against the catalog and parsed as the board will.
export async function fetchHubPackage(fw, fetchFn = fetch) {
  const label = `${fw.name} ${fw.version}`.trim();
  let res;
  try {
    res = await fetchFn(fw.url, { signal: AbortSignal.timeout(DOWNLOAD_TIMEOUT_MS) });
  } catch (e) {
    throw new Error(`Couldn't reach the Firmware Hub to download ${label} (${e.message}).`);
  }
  if (!res.ok) throw new Error(`The Firmware Hub answered ${res.status} for ${label}.`);
  const data = new Uint8Array(await res.arrayBuffer());
  if (await sha256Hex(data) !== fw.sha256) {
    throw new Error(`${label} from the Firmware Hub didn't match its catalog. Reload the page: it may have just released.`);
  }
  try {
    parsePackage(data);
  } catch (e) {
    throw new Error(`${label} isn't an FM-1 firmware Outrigger can use (${e.message}).`);
  }
  return data;
}

// ---------- The link to the board ----------
// Frames as receive.h describes them: 'O' 'R', type, length (u16 LE),
// payload, CRC-16/XMODEM of type, length and payload (u16 LE).
const MAGIC = [0x4f, 0x52];
const FRAME_MAX = 2048 + 4;
// Package bytes per data frame (receive.h kDataMax).
export const DATA_MAX = 2048;
// The ESP32-S3's USB-Serial-JTAG stalls on a single write of 4 KiB or more
// (serial.js), so every frame goes out in pieces of at most this.
export const WRITE_PIECE = 1024;
const REPLY_MS = 3000;
const STORE_MS = 60000;  // hashing and writing ~150 sectors on the board
const TRIES = 3;

export function encodeFrame(type, payload = new Uint8Array(0)) {
  const f = new Uint8Array(7 + payload.length);
  f.set([...MAGIC, type, payload.length & 0xff, payload.length >> 8]);
  f.set(payload, 5);
  const crc = crc16(f.subarray(2, 5 + payload.length));
  f[5 + payload.length] = crc & 0xff;
  f[6 + payload.length] = crc >> 8;
  return f;
}

// Finds frames in what the board sends; its log lines in between are skipped.
export class FrameReader {
  constructor() { this.buf = new Uint8Array(0); }
  push(bytes) {
    const b = new Uint8Array(this.buf.length + bytes.length);
    b.set(this.buf);
    b.set(bytes, this.buf.length);
    let at = 0;
    const out = [];
    while (b.length - at >= 2) {
      if (b[at] !== MAGIC[0] || b[at + 1] !== MAGIC[1]) { at++; continue; }
      if (b.length - at < 5) break;
      const len = b[at + 3] | (b[at + 4] << 8);
      if (len > FRAME_MAX) { at++; continue; }
      if (b.length - at < 7 + len) break;
      const crc = crc16(b.subarray(at + 2, at + 5 + len));
      if ((b[at + 5 + len] | (b[at + 6 + len] << 8)) !== crc) { at++; continue; }
      out.push({ type: b[at + 2], payload: b.slice(at + 5, at + 5 + len) });
      at += 7 + len;
    }
    // A lone trailing 'O' may start the next frame.
    this.buf = b.slice(b.length - at === 1 && b[at] !== MAGIC[0] ? b.length : at);
    return out;
  }
}

const u32 = (p, o) => (p[o] | (p[o + 1] << 8) | (p[o + 2] << 16) | (p[o + 3] << 24)) >>> 0;
const le32 = (n) => [n & 0xff, (n >> 8) & 0xff, (n >> 16) & 0xff, (n >>> 24) & 0xff];
const hex = (b) => [...b].map((x) => x.toString(16).padStart(2, "0")).join("");
const fromHex = (s) => Uint8Array.from(s.match(/../g), (x) => parseInt(x, 16));
const text = (b) => new TextDecoder().decode(b);

// Talks to the board over io = {write(bytes), read() -> bytes}, one request
// and one reply at a time. An error the board sends becomes an Error in its
// words; a reply that doesn't come is asked for again, then given up on.
export class BoardLink {
  constructor(io) {
    this.io = io;
    this.reader = new FrameReader();
    this.pending = [];
    this.reading = null;
  }

  async send(frame) {
    for (let i = 0; i < frame.length; i += WRITE_PIECE) await this.io.write(frame.subarray(i, i + WRITE_PIECE));
  }

  // The next frame, or null after ms.
  async next(ms) {
    let timer;
    const late = new Promise((r) => { timer = setTimeout(() => r(null), ms); });
    try {
      while (!this.pending.length) {
        this.reading ||= this.io.read().then((bytes) => { this.reading = null; return bytes; });
        const bytes = await Promise.race([this.reading, late]);
        if (bytes === null) return null;
        if (!bytes) throw new Error("Outrigger's USB connection closed.");
        this.pending.push(...this.reader.push(bytes));
      }
      return this.pending.shift();
    } finally {
      clearTimeout(timer);
    }
  }

  async ask(type, payload, ms = REPLY_MS) {
    const frame = encodeFrame(type, payload);
    for (let i = 0; i < TRIES; i++) {
      await this.send(frame);
      const r = await this.next(ms);
      if (!r) continue;
      if (r.type === 0x65 /* 'e' */) {
        const e = new Error(text(r.payload.subarray(1)));
        e.code = r.payload[0];
        throw e;
      }
      return r;
    }
    throw new Error("Outrigger didn't answer. Is it in Computer mode, on a build from this page?");
  }

  async hello() {
    const p = (await this.ask(0x48 /* 'H' */)).payload;
    return { protocol: p[0], packages: p[1], maxPackages: p[2], free: u32(p, 3), build: text(p.subarray(7)) };
  }

  // [{id, sha256}] for each package in the board's store.
  async list() {
    const { packages } = await this.hello();
    const out = [];
    for (let i = 0; i < packages; i++) {
      const p = (await this.ask(0x4c /* 'L' */, Uint8Array.of(i))).payload;
      out.push({ id: text(p.subarray(33)), sha256: hex(p.subarray(1, 33)) });
    }
    return out;
  }

  // Sends one checked file; "added", or "already" when the store has it.
  async add(fw, data, onProgress = () => {}) {
    const enc = new TextEncoder();
    const strs = [fw.project, fw.version, fw.name].flatMap((s) => [...enc.encode(s), 0]);
    let r = await this.ask(0x41 /* 'A' */, Uint8Array.from([...le32(data.length), ...fromHex(fw.sha256), ...strs]));
    while (r.type === 0x6b /* 'k' */) {
      const at = u32(r.payload, 0);
      onProgress(at, data.length);
      const piece = data.subarray(at, at + DATA_MAX);
      const last = at + piece.length >= data.length;
      r = await this.ask(0x44 /* 'D' */, Uint8Array.from([...le32(at), ...piece]), last ? STORE_MS : REPLY_MS);
    }
    if (r.type !== 0x64 /* 'd' */) throw new Error("Outrigger answered something this page doesn't know.");
    onProgress(data.length, data.length);
    return r.payload[0] === 1 ? "already" : "added";
  }
}
