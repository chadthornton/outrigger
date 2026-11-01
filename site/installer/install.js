// The install engine: read the board, write a build, check it, restart. It
// takes an esptool-js ESPLoader (or anything with the same few calls), so
// `make test` runs it against a fake board (tests/installer/install.test.mjs).
//
// Every write is a writeFlash of what plan.js allows, with eraseAll false and
// the flash settings kept as built: the parts, then a new store's runs once
// they verified, then that store's superblock pair once the runs verified.
// Nothing here erases.
import { planInstall, parseTable, PARTS } from "./plan.js";
import { readBuildTag, hasStore, classifyLayout, TAG_OFFSET, TAG_LENGTH } from "./board.js";
import { storeAction, fetchStarter, buildStore, sha256Hex } from "./store.js";

export const FLASH_BYTES = 16 * 1024 * 1024;
const TABLE = { offset: 0x8000, size: 0xc00 };
const nameOf = (offset) => (PARTS.find((p) => p.offset === offset) || { name: `part at 0x${offset.toString(16)}` }).name;

// About 3.4 KB of reads, nothing written: the partition table, the app's
// first bytes (the build tag) and the head of the store's two first blocks.
export async function readBoard(loader) {
  const tableBytes = await loader.readFlash(TABLE.offset, TABLE.size);
  const table = parseTable(tableBytes);
  const head = await loader.readFlash(0x10000, TAG_OFFSET + TAG_LENGTH);
  let store = false;
  let storeRange = null;
  if (table.store) {
    const [offset, size] = table.store;
    storeRange = { offset, size };
    store = hasStore(await loader.readFlash(offset, 16), await loader.readFlash(offset + 0x1000, 16));
  }
  return { tableBytes, table, layout: classifyLayout(table), tag: readBuildTag(head), store, storeRange };
}

// manifest.json and its four parts, each checked against the manifest's
// SHA-256 before anything reaches the board.
export async function fetchBuild(manifestUrl, fetchFn = fetch) {
  const res = await fetchFn(manifestUrl);
  if (!res.ok) throw new Error(`Couldn't load the build list (${manifestUrl}).`);
  const manifest = await res.json();
  const base = manifestUrl.slice(0, manifestUrl.lastIndexOf("/") + 1);
  const parts = [];
  for (const part of manifest.builds[0].parts) {
    const r = await fetchFn(base + part.path);
    if (!r.ok) throw new Error(`Couldn't download the ${nameOf(part.offset)}.`);
    const data = new Uint8Array(await r.arrayBuffer());
    if (await sha256Hex(data) !== part.sha256) throw new Error(`The ${nameOf(part.offset)} didn't download intact. Reload the page and try again.`);
    parts.push({ offset: part.offset, data });
  }
  parts.sort((a, b) => a.offset - b.offset);
  let template = null;
  if (manifest.store_template) {
    const t = await fetchFn(base + manifest.store_template);
    if (!t.ok) throw new Error(`Couldn't download the store template (${manifest.store_template}).`);
    const bytes = new Uint8Array(await t.arrayBuffer());
    if (await sha256Hex(bytes) !== manifest.store_template_sha256) {
      throw new Error("The store template didn't download intact. Reload the page and try again.");
    }
    template = JSON.parse(new TextDecoder().decode(bytes));
  }
  return { manifest, parts, template };
}

// What this install does to the board's store, worked out before anything
// is written (store.js storeAction). build comes from fetchBuild; a build
// with a store template runs the loader. Returns {action, storeWrites,
// storeWritable, downloadProblem, previousFirmware, starter}: storeWrites is
// the new store's runs ({offset, data, md5}, the superblock pair last), empty
// unless a store will be created; starter names its firmwares. When a
// download fails, downloadProblem says which and the store may not be
// written: the page must not install, so the board stays as it was and the
// next try can create the store (a board from other firmware would otherwise
// get our table over its leftovers and read as unreadable from then on).
export async function prepareStore({ loader, board, build, fetchFn = fetch }) {
  const none = { storeWrites: [], storeWritable: false, downloadProblem: null, previousFirmware: false, starter: [], unfinished: false };
  if (!build.template) return { action: "none", ...none };
  if (board.store) return { action: "keep", ...none };
  const tablePart = build.parts.find((p) => p.offset === 0x8000);
  const range = tablePart && parseTable(tablePart.data).store;
  if (!range) return { action: "none", ...none };
  const [offset, size] = range;
  if (size !== build.template.size) throw new Error("This build's store template doesn't fit its partition table (a site build problem; nothing was written).");
  // Only a board whose own table has a store needs its region checked: on a
  // board from other firmware, what's there isn't FM-1 backups.
  const hadStorePartition = Boolean(board.table.store);
  const blank = hadStorePartition && await storeIsBlank(loader, { offset, size }, build.template.blank_md5);
  const unfinished = hadStorePartition && !blank && await unfinishedStore(loader, offset, build.template);
  const action = storeAction({ runsLoader: true, hasStore: false, hadStorePartition, blank, unfinished });
  if (action !== "create") return { action, ...none };
  const previousFirmware = !hadStorePartition && board.layout !== "blank";
  const starter = (build.template.packages || []).map((p) => `${p.name} ${p.version}`);
  let downloads;
  try {
    downloads = await fetchStarter(build.template, fetchFn);
  } catch (e) {
    return { action, ...none, downloadProblem: e.message, previousFirmware, starter };
  }
  // Every download matched its SHA-256, so a store that doesn't rebuild is the site's fault, not the network's.
  const runs = await buildStore(build.template, downloads).catch((e) => {
    throw new Error(`This build's store template doesn't rebuild its store (a site build problem; nothing was written): ${e.message}.`);
  });
  const storeWrites = runs.map((r) => ({ offset: offset + r.offset, data: r.data, md5: r.md5 }));
  return { action, storeWrites, storeWritable: true, downloadProblem: null, previousFirmware, starter, unfinished };
}

// Whether the store region holds this build's store written partway and
// nothing else: every run either as the build machine made it or erased, the
// superblock pair (the last run) erased, and every gap between runs erased.
// The board computes each MD5, so nothing is read back. A template without
// gaps (an older build) never matches.
export async function unfinishedStore(loader, offset, template) {
  if (!template.gaps || !template.runs.length) return false;
  const superblock = template.runs.at(-1);
  for (const r of template.runs) {
    const got = await loader.flashMd5sum(offset + r.offset, r.size);
    if (got !== r.blank_md5 && (r === superblock || got !== r.md5)) return false;
  }
  for (const g of template.gaps) {
    if (await loader.flashMd5sum(offset + g.offset, g.size) !== g.blank_md5) return false;
  }
  return true;
}

// Whether the store region is factory-blank: the board computes the MD5 of
// the whole region (seconds, where reading it back takes minutes) and it's
// compared with an erased region's, from store-template.json.
export async function storeIsBlank(loader, storeRange, blankMd5) {
  return (await loader.flashMd5sum(storeRange.offset, storeRange.size)) === blankMd5;
}

// Writes the build, and with storeWrites a new store (only where
// prepareStore cleared it). board comes from readBoard on this connection.
// onStep({step, part?, written?, total?}): "write" per part, "verify", then
// "verify store" when a store was written. Returns {plan, storeProblem}:
// storeProblem says why a new store didn't verify (the program is in place
// and checked by then, so the install still finishes).
//
// The store's last write is its superblock pair, the blocks that make it a
// store (board.js hasStore). It goes in only after every other run matches
// the MD5 the build machine computed, so a cable pulled mid-store leaves a
// region that reads as no store, never as a store with parts missing.
export async function install({ loader, board, manifest, parts, storeWrites = [], storeWritable = false, onStep = () => {} }) {
  const plan = planInstall({ parts, boardTable: board.tableBytes, flashBytes: FLASH_BYTES, storeWrites, storeWritable });
  if (!plan.ok) throw new Error(`Nothing was written: ${plan.problems.join("; ")}.`);
  const program = plan.writes.filter((w) => w.name !== "store");
  const store = plan.writes.filter((w) => w.name === "store");
  const superblock = store.pop();

  // The program first, checked before any of the store goes in: a part that
  // doesn't verify leaves a factory-blank store region blank.
  await write(loader, program, onStep);
  onStep({ step: "verify" });
  const md5 = new Map(manifest.builds[0].parts.map((p) => [p.offset, p.md5]));
  for (const w of program) {
    const got = await loader.flashMd5sum(w.offset, w.data.length);
    if (got !== md5.get(w.offset)) {
      throw new Error(`The ${w.name} didn't read back as written. Your settings and backups weren't touched; install again.`);
    }
  }
  if (!superblock) return { plan, storeProblem: null };
  await write(loader, store, onStep);
  onStep({ step: "verify store" });
  let bad = await settle(loader, store, onStep);
  if (!bad) {
    await write(loader, [superblock], onStep);
    bad = await settle(loader, [superblock], onStep);
  }
  if (bad) return { plan, storeProblem: `the new store didn't verify at 0x${bad.offset.toString(16)}; email feedback@outrigger.music before installing again` };
  return { plan, storeProblem: null };
}

async function write(loader, writes, onStep) {
  await loader.writeFlash({
    fileArray: writes.map((w) => ({ data: w.data, address: w.offset })),
    flashMode: "keep",
    flashFreq: "keep",
    flashSize: "keep",
    eraseAll: false,
    compress: true,
    reportProgress: (i, written, total) => onStep({ step: "write", part: writes[i].name, written, total }),
  });
}

// Checks store writes by the board's MD5 and writes the ones that differ once
// more (the region is the new store's alone). The first that still differs,
// or null.
async function settle(loader, writes, onStep) {
  const differ = async (ws) => {
    const out = [];
    for (const w of ws) if (await loader.flashMd5sum(w.offset, w.data.length) !== w.md5) out.push(w);
    return out;
  };
  let bad = await differ(writes);
  if (bad.length) {
    await write(loader, bad, onStep);
    bad = await differ(bad);
  }
  return bad[0] || null;
}

// An RTC watchdog reset, as esptool's --after watchdog-reset: unlike the RTS
// reset it starts the program even when the board came up through BOOT+RST.
// Registers from the ESP32-S3's soc/rtc_cntl_reg.h.
const RTC_CNTL = 0x60008000;
const WDT_CONFIG0 = RTC_CNTL + 0x98;
const WDT_CONFIG1 = RTC_CNTL + 0x9c;
const WDT_WPROTECT = RTC_CNTL + 0xb0;
const WDT_WKEY = 0x50d83aa1;

export async function restart(loader) {
  await loader.writeReg(WDT_WPROTECT, WDT_WKEY);
  await loader.writeReg(WDT_CONFIG1, 2000);
  await loader.writeReg(WDT_CONFIG0, ((1 << 31) | (5 << 28) | (1 << 8) | 2) >>> 0); // on; stage 0 resets the system
  await loader.writeReg(WDT_WPROTECT, 0);
}

// After a verified install: restart the board and let go of the port. The
// install is done by then, so nothing here can fail it. The watchdog fires
// within milliseconds of being armed, so the lock write and the disconnect
// may meet a port that has already dropped.
export async function finish({ loader, transport, timeoutMs = 2000 }) {
  try {
    await restart(loader);
  } catch {
    // the chip reset under us, or never armed; RST still starts it
  }
  try {
    await Promise.race([transport.disconnect(), new Promise((r) => setTimeout(r, timeoutMs))]);
  } catch {
    // already gone
  }
}
