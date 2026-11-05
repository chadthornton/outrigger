// Setting up the store on a board that has none, so the loader can switch
// FM-1 firmware without the Mac setup (`make flash-store`). Pure, so `make
// test` runs it (tests/installer/store.test.mjs).
//
// The site publishes store-template.json (tools/store_template.py): a store
// holding JieLi's loader and a starter set of FM-1 firmwares, without their
// bytes. The visitor's browser downloads the loader from the pinned
// jl-uboot-tool commit on GitHub and each package from its pinned URL (stock
// V15 from M-VAVE's repo on GitHub), checks every SHA-256, and fills the store
// in. This site never hosts the loader or any firmware.

const hex = (bytes) => [...bytes].map((b) => b.toString(16).padStart(2, "0")).join("");
export const sha256Hex = async (bytes) => hex(new Uint8Array(await crypto.subtle.digest("SHA-256", bytes)));
// GitHub gets this long before the page says the loader couldn't be downloaded.
const LOADER_TIMEOUT_MS = 20000;
// Each starter package is about 700 KB.
const PACKAGE_TIMEOUT_MS = 30000;
const SECTOR = 0x1000;
const fromHex = (s) => Uint8Array.from(s.match(/../g), (b) => parseInt(b, 16));

// What to do with the board's store (Chad, 2026-10-31), decided by the
// board's own partition table, read before anything is written:
// - none: the build doesn't run the loader, so the store isn't touched;
// - keep: a store exists, with backups or not; it's never written;
// - create: the board's table has no store (it ran other firmware, so what's
//   there is that firmware's, not FM-1 backups), or it has one and the region
//   is factory-blank (all 0xFF): write a store;
//   It's also create when the region holds an install of this build's store
//   that was cut short (Chad, 2026-11-01): only this installer's own writes,
//   without the superblock pair, which a store with backups always has.
// - unreadable: the board's table has a store, the region holds data, and no
//   store this page can read: where backups may have lived. Stop.
export function storeAction({ runsLoader, hasStore, hadStorePartition, blank, unfinished = false }) {
  if (!runsLoader) return "none";
  if (hasStore) return "keep";
  if (!hadStorePartition) return "create";
  return blank || unfinished ? "create" : "unreadable";
}

// The loader from GitHub, or an Error that says why not, in words for the page.
export async function fetchLoader(template, fetchFn = fetch) {
  let res;
  try {
    res = await fetchFn(template.loader.url, { signal: AbortSignal.timeout(LOADER_TIMEOUT_MS) });
  } catch (e) {
    throw new Error(`couldn't reach GitHub to download JieLi's loader (${e.message})`);
  }
  if (!res.ok) throw new Error(`GitHub answered ${res.status} for JieLi's loader`);
  const blob = new Uint8Array(await res.arrayBuffer());
  if (await sha256Hex(blob) !== template.loader.sha256 || blob.length !== template.blob_size) {
    throw new Error("the loader downloaded from GitHub didn't match the one this installer expects");
  }
  return blob;
}

// One starter package from its pinned URL, checked and decoded to its app.
// p.from names the site for messages ("GitHub").
async function fetchPackage(p, fetchFn) {
  const label = `${p.name} ${p.version}`;
  const from = p.from || "the download site";
  let res;
  try {
    res = await fetchFn(p.url, { signal: AbortSignal.timeout(PACKAGE_TIMEOUT_MS) });
  } catch (e) {
    throw new Error(`couldn't reach ${from} to download ${label} (${e.message})`);
  }
  if (!res.ok) throw new Error(`${from} answered ${res.status} for ${label}`);
  const data = new Uint8Array(await res.arrayBuffer());
  if (await sha256Hex(data) !== p.sha256) throw new Error(`${label} downloaded from ${from} didn't match the one this installer expects`);
  const { app } = parsePackage(data);
  if (await sha256Hex(app) !== p.app_sha256) throw new Error(`${label} didn't decode to the firmware this installer expects`);
  return app;
}

// The loader and every starter package's app ({loader, apps: id -> app}),
// or an Error naming the first one that didn't come, in words for the page.
export async function fetchStarter(template, fetchFn = fetch) {
  const [loader, ...apps] = await Promise.all([fetchLoader(template, fetchFn),
    ...template.packages.map((p) => fetchPackage(p, fetchFn))]);
  return { loader, apps: new Map(template.packages.map((p, i) => [p.id, apps[i]])) };
}

// The store's runs as the installer writes them, [{offset, data, md5}]
// relative to the store's start, the superblock pair last: the template's
// blocks, each firmware sector found by its SHA-256 among the apps, the
// loader copied over its placeholder. Refuses (nothing to write) unless the
// whole is the store tools/store_template.py built and mounted.
export async function buildStore(template, { loader, apps }) {
  const sectors = new Map();
  for (const app of apps.values()) {
    for (let at = 0; at < app.length; at += SECTOR) {
      const s = app.subarray(at, at + SECTOR);
      sectors.set(await sha256Hex(s), s);
    }
  }
  const blocks = new Map();
  for (const b of template.blocks) {
    const data = new Uint8Array(SECTOR).fill(0xff);
    if (b.sector) {
      const s = sectors.get(b.sector);
      if (!s) throw new Error("a firmware sector the store needs is missing from the downloads");
      data.set(s);
    } else if (b.hex) {
      data.set(fromHex(b.hex));
    }
    blocks.set(b.offset, data);
  }
  for (const [img, src, n] of template.extents) {
    const start = img - (img % SECTOR);
    blocks.get(start).set(loader.subarray(src, src + n), img - start);
  }
  const sorted = [...blocks].sort((a, b) => a[0] - b[0]);
  const whole = new Uint8Array(sorted.length * (4 + SECTOR));
  sorted.forEach(([off, data], i) => {
    new DataView(whole.buffer).setUint32(i * (4 + SECTOR), off, true);
    whole.set(data, i * (4 + SECTOR) + 4);
  });
  if (await sha256Hex(whole) !== template.sha256) throw new Error("the new store didn't match the one this installer expects");
  return template.runs.map((r) => {
    const data = new Uint8Array(r.size);
    for (let at = 0; at < r.size; at += SECTOR) data.set(blocks.get(r.offset + at), at);
    return { offset: r.offset, data, md5: r.md5 };
  });
}

// An FM-1 package (.fwsc) decoded to {identity, app}: the 0x4000-0x93000 app
// area its flash.bin carries, with lib/fm1_loader's checks (package.cpp; the
// Python port is tools/fm1lib.py parse_package). The parsing follows
// MvaveFM1Unbricker (MIT, Michael P Jones): see NOTICE.
const FWSC_BLOCK = 0x2f;
const FWSC_MARKED = 20;
const FWSC_NO_CHAR = 0x7d;
const APP_START = 0x4000;
const APP_END = 0x93000;

export function crc16(data, c = 0) {
  for (const b of data) {
    c ^= b << 8;
    for (let i = 0; i < 8; i++) c = (c & 0x8000 ? (c << 1) ^ 0x1021 : c << 1) & 0xffff;
  }
  return c;
}

function jlEnc(buf) {
  const out = Uint8Array.from(buf);
  let k = 0xffff;
  for (let i = 0; i < out.length; i++) {
    out[i] ^= k & 0xff;
    k = ((k << 1) ^ (k & 0x8000 ? 0x1021 : 0)) & 0xffff;
  }
  return out;
}

const u16 = (p, o) => p[o] | (p[o + 1] << 8);
const u32 = (p, o) => (u16(p, o) | (u16(p, o + 2) << 16)) >>> 0;

export function parsePackage(data) {
  const stride = FWSC_BLOCK + 1;
  if (data.length < stride * FWSC_MARKED) throw new Error("too short to be an FM-1 package");
  const image = new Uint8Array(data.length - FWSC_MARKED);
  let identity = "";
  for (let i = 0; i < FWSC_MARKED; i++) {
    image.set(data.subarray(i * stride, i * stride + FWSC_BLOCK), i * FWSC_BLOCK);
    const mark = data[i * stride + FWSC_BLOCK];
    if (mark !== FWSC_NO_CHAR) identity += String.fromCharCode((mark - i - 1) & 0xff);
  }
  image.set(data.subarray(stride * FWSC_MARKED), FWSC_BLOCK * FWSC_MARKED);
  if (image.length < 0x400) throw new Error("too short to be an FM-1 package");
  const hdr = jlEnc(image.subarray(0, 0x40));
  if (crc16(hdr.subarray(2, 0x40)) !== u16(hdr, 0)) throw new Error("UFW header CRC fails");
  const n = u16(hdr, 8);
  if (n === 0 || n > 11) throw new Error("UFW header lists a bad entry count");
  if (crc16(image.subarray(0x40, 0x40 + n * 0x50)) !== u16(hdr, 2)) throw new Error("UFW entry list CRC fails");
  let fl = null;
  for (let i = 0; i < n; i++) {
    const e = jlEnc(image.subarray(0x40 + i * 0x50, 0x90 + i * 0x50));
    if (u16(e, 0) === 0) fl = [u32(e, 8), u32(e, 12)];
  }
  if (!fl) throw new Error("no flash.bin entry");
  const [off, length] = fl;
  if (off + length > image.length) throw new Error("flash.bin runs past the package");
  if (length < APP_END) throw new Error("flash.bin doesn't reach 0x93000");
  if (!/^FM-1_\d{3,15}$/.test(identity)) throw new Error("identity isn't FM-1_ and 3-15 digits");
  const app = image.slice(off + APP_START, off + APP_END);
  if (app.every((b) => b === 0xff) || app.every((b) => b === 0)) throw new Error("app area is blank");
  return { identity, app };
}
