// Setting up the store on a board that has none, so the loader can switch
// FM-1 firmware without the Mac setup (`make flash-store`). Pure, so `make
// test` runs it (tests/installer/store.test.mjs).
//
// The site publishes store-template.json (tools/store_template.py): the store
// `make store-image` builds, with a placeholder where JieLi's loader goes. The
// visitor's browser downloads the loader from the pinned jl-uboot-tool commit
// on GitHub, checks its SHA-256, and copies it over the placeholder. This site
// never hosts the loader.

const hex = (bytes) => [...bytes].map((b) => b.toString(16).padStart(2, "0")).join("");
export const sha256Hex = async (bytes) => hex(new Uint8Array(await crypto.subtle.digest("SHA-256", bytes)));
// GitHub gets this long before the page says the loader couldn't be downloaded.
const LOADER_TIMEOUT_MS = 20000;
const fromHex = (s) => Uint8Array.from(s.match(/../g), (b) => parseInt(b, 16));

// What to do with the board's store (Chad, 2026-10-31), decided by the
// board's own partition table, read before anything is written:
// - none: the build doesn't run the loader, so the store isn't touched;
// - keep: a store exists, with backups or not; it's never written;
// - create: the board's table has no store (it ran other firmware, so what's
//   there is that firmware's, not FM-1 backups), or it has one and the region
//   is factory-blank (all 0xFF): write a store;
// - unreadable: the board's table has a store, the region holds data, and no
//   store this page can read: where backups may have lived. Stop.
export function storeAction({ runsLoader, hasStore, hadStorePartition, blank }) {
  if (!runsLoader) return "none";
  if (hasStore) return "keep";
  if (!hadStorePartition) return "create";
  return blank ? "create" : "unreadable";
}

// [{offset, data}] relative to the store's start: the template's blocks with
// the loader copied over the placeholder.
export function spliceStore(template, blob) {
  const blocks = new Map(template.blocks.map((b) => [b.offset, fromHex(b.hex)]));
  for (const [img, src, n] of template.extents) {
    const start = img - (img % 0x1000);
    blocks.get(start).set(blob.subarray(src, src + n), img - start);
  }
  return [...blocks].sort((a, b) => a[0] - b[0]).map(([offset, data]) => ({ offset, data }));
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
