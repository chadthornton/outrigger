// The install engine: read the board, write a build, check it, restart. It
// takes an esptool-js ESPLoader (or anything with the same few calls), so
// `make test` runs it against a fake board (tests/installer/install.test.mjs).
//
// The only write is one writeFlash of the parts plan.js allows, with
// eraseAll false and the flash settings kept as built. Nothing here erases.
import { planInstall, parseTable, PARTS } from "./plan.js";
import { readBuildTag, hasStore, classifyLayout, TAG_OFFSET, TAG_LENGTH } from "./board.js";

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
  return { manifest, parts };
}

// Writes the build. board comes from readBoard on this same connection.
// onStep({step, part?, written?, total?}): "write" per part, then "verify".
export async function install({ loader, board, manifest, parts, onStep = () => {} }) {
  const plan = planInstall({ parts, boardTable: board.tableBytes, flashBytes: FLASH_BYTES });
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
  for (const w of plan.writes) {
    const got = await loader.flashMd5sum(w.offset, w.data.length);
    if (got !== md5.get(w.offset)) {
      throw new Error(`The ${w.name} didn't read back as written. Your settings and backups weren't touched; install again.`);
    }
  }
  return plan;
}

// The whole store, read back for the laptop. onProgress(done, total).
export async function backupStore(loader, storeRange, onProgress = () => {}) {
  let done = 0;
  return loader.readFlash(storeRange.offset, storeRange.size, (packet) => {
    done += packet.length;
    onProgress(Math.min(done, storeRange.size), storeRange.size);
  });
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
