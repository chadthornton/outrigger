// The install engine: read the board, write a build, check it, restart. It
// takes an esptool-js ESPLoader (or anything with the same few calls), so
// `make test` runs it against a fake board (tests/installer/install.test.mjs).
//
// The only write is one writeFlash of the parts plan.js allows, with
// eraseAll false and the flash settings kept as built. Nothing here erases.
import { planInstall, parseTable, PARTS } from "./plan.js";
import { readBuildTag, hasStore, classifyLayout, TAG_OFFSET, TAG_LENGTH } from "./board.js";
import { storeAction, spliceStore, fetchLoader } from "./store.js";

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
    const digest = new Uint8Array(await crypto.subtle.digest("SHA-256", data));
    const hex = [...digest].map((b) => b.toString(16).padStart(2, "0")).join("");
    if (hex !== part.sha256) throw new Error(`The ${nameOf(part.offset)} didn't download intact. Reload the page and try again.`);
    parts.push({ offset: part.offset, data });
  }
  parts.sort((a, b) => a.offset - b.offset);
  let template = null;
  if (manifest.store_template) {
    const t = await fetchFn(base + manifest.store_template);
    if (!t.ok) throw new Error(`Couldn't download the store template (${manifest.store_template}).`);
    template = await t.json();
  }
  return { manifest, parts, template };
}

// What this install does to the board's store, worked out before anything
// is written (store.js storeAction). build comes from fetchBuild; a build
// with a store template runs the loader. Returns {action, storeWrites,
// storeBlank, loaderProblem}: storeWrites is empty unless a store will be
// created; loaderProblem says why it won't be when GitHub didn't deliver.
export async function prepareStore({ loader, board, build, fetchFn = fetch }) {
  const none = { storeWrites: [], storeBlank: false, loaderProblem: null };
  if (!build.template) return { action: "none", ...none };
  if (board.store) return { action: "keep", ...none };
  const tablePart = build.parts.find((p) => p.offset === 0x8000);
  const range = tablePart && parseTable(tablePart.data).store;
  if (!range) return { action: "none", ...none };
  const [offset, size] = range;
  const blank = size === build.template.size && await storeIsBlank(loader, { offset, size }, build.template.blank_md5);
  const action = storeAction({ runsLoader: true, hasStore: false, blank });
  if (action !== "create") return { action, ...none };
  try {
    const blob = await fetchLoader(build.template, fetchFn);
    const storeWrites = spliceStore(build.template, blob).map((b) => ({ offset: offset + b.offset, data: b.data }));
    return { action, storeWrites, storeBlank: true, loaderProblem: null };
  } catch (e) {
    return { action, storeWrites: [], storeBlank: true, loaderProblem: e.message };
  }
}

// Whether the store region is factory-blank: the board computes the MD5 of
// the whole region (seconds, where reading it back takes minutes) and it's
// compared with an erased region's, from store-template.json.
export async function storeIsBlank(loader, storeRange, blankMd5) {
  return (await loader.flashMd5sum(storeRange.offset, storeRange.size)) === blankMd5;
}

// Writes the build, and with storeWrites a new store (only onto a region
// storeIsBlank found blank). board comes from readBoard on this connection.
// onStep({step, part?, written?, total?}): "write" per part, "verify", then
// "verify store" when a store was written.
export async function install({ loader, board, manifest, parts, storeWrites = [], storeBlank = false, onStep = () => {} }) {
  const plan = planInstall({ parts, boardTable: board.tableBytes, flashBytes: FLASH_BYTES, storeWrites, storeBlank });
  if (!plan.ok) throw new Error(`Nothing was written: ${plan.problems.join("; ")}.`);

  await loader.writeFlash({
    fileArray: plan.writes.map((w) => ({ data: w.data, address: w.offset })),
    flashMode: "keep",
    flashFreq: "keep",
    flashSize: "keep",
    eraseAll: false,
    compress: true,
    reportProgress: (i, written, total) => onStep({ step: "write", part: plan.writes[i].name, written, total }),
  });

  onStep({ step: "verify" });
  const md5 = new Map(manifest.builds[0].parts.map((p) => [p.offset, p.md5]));
  for (const w of plan.writes.filter((w) => w.name !== "store")) {
    const got = await loader.flashMd5sum(w.offset, w.data.length);
    if (got !== md5.get(w.offset)) {
      throw new Error(`The ${w.name} didn't read back as written. Your settings and backups weren't touched; install again.`);
    }
  }
  // The store blocks hold the loader the browser downloaded, so the manifest
  // has no MD5 for them: read them back instead (a few KiB).
  const store = plan.writes.filter((w) => w.name === "store");
  if (store.length) onStep({ step: "verify store" });
  for (const w of store) {
    const got = await loader.readFlash(w.offset, w.data.length);
    if (got.length !== w.data.length || got.some((b, i) => b !== w.data[i])) {
      throw new Error(`The new store didn't read back as written at 0x${w.offset.toString(16)}. Email feedback@outrigger.music before installing again.`);
    }
  }
  return plan;
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
