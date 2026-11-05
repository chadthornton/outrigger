// The firmware library page (site/library/index.html): the catalog and the
// board's store as rows in three tabs, the search, favorites, and the cart of
// adds that one Apply sends over one connection. Pure, so `make test` runs it
// (tests/installer/library.test.mjs); library_area.js draws it and talks to
// the board through add.js.
//
// What the board can say comes from lib/fm1_loader/src/receive.h: 'H' (how
// many packages, the most it takes, free bytes) and 'L' (each package's id and
// file SHA-256). It only adds: nothing on this page removes a package yet.

// The board's limits (receive.h): kMaxPackages; an add needs
// kBackupReserve + kPackageRoom free before it starts. Sectors: layout.h
// kSectors (256) and kAppSectors (143), 4 KiB each.
export const MAX_PACKAGES = 10;
const SECTOR = 0x1000;
export const BACKUP_RESERVE = (256 + 16) * SECTOR;
export const PACKAGE_ROOM = (143 + 4) * SECTOR;

// Firmwares the riskier-firmwares change makes installable after a warning
// (its families_gen.js RISKIER). Until a board's loader takes them, add.js
// holds them and the page shows them as coming soon; once add.js marks them
// riskier, they're addable and this list matches nothing held.
const RISKIER_SOON = ["clipper", "floyd", "floyd-felucca", "floyd-sloop", "fm1quest", "karma", "melodee", "nmsfm",
  "sloop", "sloopbanks", "spampy", "tessera", "tessera-fm-1-83773022", "winampfm"];

// The id the board stores a package under (receive.cpp, as tools/store_add.py
// names catalog packages): the project alone when it has no version or already
// ends in it, else project-version.
export function boardId(project, version) {
  return !version || (project.length > version.length && project.endsWith(version)) ? project : `${project}-${version}`;
}

const isStock = (id) => id === "stock" || id.startsWith("stock-");

// rows for the tabs. lists: addableFirmwares(catalog) ({addable, held});
// catalog: the catalog it came from, for authors and descriptions; stored: the
// board's [{id, sha256}], or null when no board is connected.
// Returns {board, available, riskier, kept}: kept is the stock packages.
export function libraryRows(lists, catalog, stored) {
  const info = new Map((catalog.firmwares || []).map((e) => [e.id, e]));
  const more = (project) => {
    const e = info.get(project) || {};
    return { author: e.author || "", what: e.what || e.description || "", release: e.release || "", date: e.date || "" };
  };
  const addable = lists.addable.filter((f) => f.project !== "stock");
  const ids = new Set((stored || []).map((p) => p.id));
  // Which project each stored package belongs to: the catalog's exact id, else
  // the longest project it starts with (projects and versions both have
  // hyphens, so the id isn't split).
  const projects = [...addable.map((f) => f.project), ...lists.held.map((f) => f.project)]
    .sort((a, b) => b.length - a.length);
  const owner = (id) => addable.find((f) => boardId(f.project, f.version) === id)?.project
    || projects.find((p) => id === p || id.startsWith(`${p}-`));
  const owned = new Set();

  const board = [];
  const kept = [];
  for (const p of stored || []) {
    if (isStock(p.id)) { kept.push({ key: p.id, name: "M-VAVE stock", version: p.id.slice(6) || "V15" }); continue; }
    const project = owner(p.id);
    if (project) owned.add(project);
    const fw = addable.find((f) => f.project === project);
    const held = lists.held.find((f) => f.project === project);
    const latest = fw && boardId(fw.project, fw.version);
    const version = project && p.id !== project ? p.id.slice(project.length + 1) : fw && latest === p.id ? fw.version : "";
    const row = { key: p.id, project: project || p.id, name: fw?.name || held?.name || p.id, version, ...more(project) };
    if (fw && latest === p.id) board.push({ ...row, state: "current", fw });
    // An update only for the newest stored copy: never twice for one project.
    else if (fw && !ids.has(latest) && !board.some((r) => r.state === "update" && r.project === project)) {
      board.push({ ...row, state: "update", fw });
    } else board.push({ ...row, state: project ? "old" : "unknown" });
  }

  const free = addable.filter((f) => !owned.has(f.project));
  const row = (f, state) => ({ key: f.project, project: f.project, name: f.name, version: f.version, ...more(f.project), state, fw: f });
  const available = free.filter((f) => !f.riskier).map((f) => row(f, "available"));
  const riskier = [
    ...free.filter((f) => f.riskier).map((f) => row(f, "riskier")),
    ...lists.held.filter((f) => RISKIER_SOON.includes(f.project) || RISKIER_SOON.includes(info.get(f.project)?.entry))
      .map((f) => ({ key: f.project, project: f.project, name: f.name, version: info.get(f.project)?.version || "",
        ...more(f.project), state: "soon" })),
    // Held for what they write, not for a missing license or a broken file.
    ...lists.held.filter((f) => !RISKIER_SOON.includes(f.project) && !RISKIER_SOON.includes(info.get(f.project)?.entry)
      && /Bluetooth|clear or restore/.test(f.why))
      .map((f) => ({ key: f.project, project: f.project, name: f.name, version: "", ...more(f.project), state: "held", why: f.why })),
  ];
  return { board, available, riskier, kept };
}

// ---------- Search and favorites ----------

export function matches(row, q) {
  const words = q.trim().toLowerCase();
  return !words || `${row.name} ${row.author} ${row.what}`.toLowerCase().includes(words);
}

// The rows that match, favorites first, each group in its own order.
export function shownRows(rows, q, favs = []) {
  const fav = new Set(favs);
  const hit = rows.filter((r) => matches(r, q));
  return [...hit.filter((r) => fav.has(r.key)), ...hit.filter((r) => !fav.has(r.key))];
}

export function countLine(shown, total, q) {
  if (q.trim()) return `Showing ${shown} of ${total}`;
  return `${total} ${total === 1 ? "firmware" : "firmwares"}`;
}

export function toggle(list, key) {
  return list.includes(key) ? list.filter((k) => k !== key) : [...list, key];
}

// ---------- The cart and the meter ----------

// How many more packages the board takes: its count limit, and the bytes each
// add needs before it starts (one PACKAGE_ROOM per add, the most a package
// uses, so the page never offers one the board will refuse).
export function room(hello) {
  const byCount = hello.maxPackages - hello.packages;
  const byBytes = Math.floor((hello.free - BACKUP_RESERVE) / PACKAGE_ROOM);
  return Math.max(0, Math.min(byCount, byBytes));
}

// The adds still possible with the cart as it is.
export const canAdd = (hello, cart) => cart.length < room(hello);

// One cell per package the board can hold: stock, the others stored, the cart,
// free, and slots the count allows but memory doesn't ("full").
export function meter(hello, stockCount, cart) {
  const total = hello.maxPackages;
  const open = room(hello);
  const cells = [];
  for (let i = 0; i < total; i++) {
    const kind = i < stockCount ? "kept"
      : i < hello.packages ? "installed"
        : i < hello.packages + Math.min(cart.length, open) ? "cart"
          : i < hello.packages + open ? "free" : "full";
    cells.push(kind);
  }
  return cells;
}

const mb = (bytes) => (bytes / 1048576).toFixed(1);

export function storageLine(hello, cart) {
  const left = room(hello) - cart.length;
  const after = Math.max(0, hello.free - cart.length * PACKAGE_ROOM);
  return `${hello.packages} of ${hello.maxPackages} firmwares · about ${mb(after)} MB free · room for ${Math.max(0, left)} more`;
}

// Applying the cart one add at a time over one connection: results[i] is
// "added", "already" or an Error for cart[i], and stops at the first Error.
// Returns what landed, what's left in the cart (the failed one and every one
// after it), and the error.
export function applyOutcome(cart, results) {
  const at = results.findIndex((r) => r instanceof Error);
  const done = at < 0 ? results.length : at;
  return { landed: cart.slice(0, done), left: cart.slice(done), error: at < 0 ? null : results[at] };
}

// The board's refusals (receive.h ReceiveError) in the page's words, when its
// own words don't say what to do.
export function addError(e, label) {
  if (e.code === 4) return `${label} wasn't added: this Outrigger doesn't take it yet. Update Outrigger from the home page, then try again.`;
  if (e.code === 5) return `${label} wasn't added: Outrigger has a different file under this version. It may have been re-released; removing the old one isn't possible from this page yet.`;
  return `${label} wasn't added. ${e.message}`;
}
