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
const fromHex = (s) => Uint8Array.from(s.match(/../g), (b) => parseInt(b, 16));

// What to do with the board's store (Chad, 2026-10-31):
// - none: the build doesn't run the loader, so the store isn't touched;
// - keep: a store exists, with backups or not; it's never written;
// - create: the store region is factory-blank (all 0xFF), so write a store;
// - unreadable: the region holds data but no store this page can read; stop.
export function storeAction({ runsLoader, hasStore, blank }) {
  if (!runsLoader) return "none";
  if (hasStore) return "keep";
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
    res = await fetchFn(template.loader.url);
  } catch (e) {
    throw new Error(`couldn't reach GitHub to download JieLi's loader (${e.message})`);
  }
  if (!res.ok) throw new Error(`GitHub answered ${res.status} for JieLi's loader`);
  const blob = new Uint8Array(await res.arrayBuffer());
  const digest = hex(new Uint8Array(await crypto.subtle.digest("SHA-256", blob)));
  if (digest !== template.loader.sha256 || blob.length !== template.blob_size) {
    throw new Error("the loader downloaded from GitHub didn't match the one this installer expects");
  }
  return blob;
}
