// The firmware library page (site/library/index.html): the catalog and the
// board's store as rows in three tabs, the search, favorites, and the cart of
// adds that one Apply sends over one connection. Pure, so `make test` runs it
// (tests/installer/library.test.mjs); library_area.js draws it and talks to
// the board through add.js.
//
// What the board can say comes from lib/fm1_loader/src/receive.h: 'H' (how
// many packages, the most it takes, free bytes) and 'L' (each package's id and
// file SHA-256); on protocol 2, 'R' removes a package and 'I' says what the
// loader knows about the FM-1 it saw last.

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
// board's [{id, sha256}], or null when no board is connected; running: the
// package id the FM-1 runs ('I'), or "" when unknown.
// Returns {board, available, riskier, kept}: kept is the stock packages.
export function libraryRows(lists, catalog, stored, running = "") {
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
    const row = { key: p.id, project: project || p.id, name: fw?.name || held?.name || p.id, version, ...more(project),
      running: p.id === running };
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
// uses, so the page never offers one the board will refuse). removes: how many
// queued removes go first. Each frees a place in the count; the bytes it frees
// aren't counted, since its sectors may be shared with a backup or a save.
export function room(hello, removes = 0) {
  const byCount = hello.maxPackages - hello.packages + removes;
  const byBytes = Math.floor((hello.free - BACKUP_RESERVE) / PACKAGE_ROOM);
  return Math.max(0, Math.min(byCount, byBytes));
}

// The adds still possible with the cart as it is.
export const canAdd = (hello, cart, removes = []) => cart.length < room(hello, removes.length);

// A row the page offers to remove: on the board, not stock, not what the FM-1 runs.
export const removable = (row) => !!row && !row.running && ["current", "update", "old", "unknown"].includes(row.state);

// Apply's order: removes first, so the adds have their room. The page
// downloads every add before it sends the first step, so a failed download
// never leaves a board with an old copy removed and its update missing.
export const steps = (removes, adds) => [...removes.map((id) => ({ remove: id })), ...adds.map((fw) => ({ add: fw }))];

// Update on a board row: queues the newer version and, unless the FM-1 runs
// the stored copy, a remove of that copy, so updates don't pile up versions.
// Again (Undo) takes both back out. Returns the new {cart, removes}.
export function queueUpdate({ cart, removes }, row) {
  if (cart.includes(row.fw)) return { cart: cart.filter((f) => f !== row.fw), removes: removes.filter((id) => id !== row.key) };
  return { cart: [...cart, row.fw], removes: removable(row) && !removes.includes(row.key) ? [...removes, row.key] : removes };
}

// A board row is being replaced: its update is in the cart and its remove queued.
export const replacing = (row, cart, removes) => !!row.fw && cart.includes(row.fw) && removes.includes(row.key);

// Why a board row can't go, on that row: what the board said when it refused
// (refused: {id: words}, from Apply), or that the FM-1 runs it. null when it can.
export function removeWhy(row, refused = {}) {
  if (refused[row.key]) return refused[row.key];
  if (row.running) return "Running now. Switch the FM-1 to another firmware to remove it.";
  return null;
}

// The words a remove's refusal puts on its row (receive.h InUse), or null for
// a failure that stops Apply.
export const refusedWhy = (e) => (e.code === 13 ? "Outrigger kept it: your FM-1 runs it, or an install of it isn't finished." : null);

// The step line while one runs, with how long it has run, so a slow remove or
// store still shows it's working.
export const elapsed = (step, ms) => (ms < 3000 ? step : `${step} ${Math.floor(ms / 1000)} s`);

// One cell per package the board can hold: stock, the others stored, those
// queued to go ("removing"), the cart, free, and slots the count allows but
// memory doesn't ("full"). When the cart fills places a remove frees, those
// removing cells show as cart.
export function meter(hello, stockCount, cart, removes = []) {
  const gone = removes.length;
  const open = room(hello, gone);
  const adds = Math.min(cart.length, open);
  const reuse = Math.max(0, hello.packages + adds - hello.maxPackages);  // removing places the cart fills
  const run = (kind, n) => Array(Math.max(0, n)).fill(kind);
  const cells = [...run("kept", stockCount), ...run("installed", hello.packages - stockCount - gone),
    ...run("removing", gone - reuse), ...run("cart", adds)];
  const free = Math.min(hello.maxPackages - cells.length, open - adds - (gone - reuse));
  return [...cells, ...run("free", free), ...run("full", hello.maxPackages - cells.length - Math.max(0, free))];
}

const mb = (bytes) => (bytes / 1048576).toFixed(1);

export function storageLine(hello, cart, removes = []) {
  const left = room(hello, removes.length) - cart.length;
  const after = Math.max(0, hello.free - cart.length * PACKAGE_ROOM);
  return `${hello.packages} of ${hello.maxPackages} firmwares · about ${mb(after)} MB free · room for ${Math.max(0, left)} more`;
}

// Applying the plan one step at a time over one connection: results[i] is
// "removed", "added", "already", "kept" (a remove the board refused because
// the FM-1 needs it: nothing changed, so the rest still run) or an Error for
// plan[i], and stops at the first Error. Returns what landed, what the board
// kept, what's left (the failed one and every one after it), and the error.
export function applyOutcome(plan, results) {
  const at = results.findIndex((r) => r instanceof Error);
  const done = at < 0 ? results.length : at;
  const ran = plan.slice(0, done);
  return { landed: ran.filter((_, i) => results[i] !== "kept"), kept: ran.filter((_, i) => results[i] === "kept"),
    left: plan.slice(done), error: at < 0 ? null : results[at] };
}

// The board's refusals (receive.h ReceiveError) in the page's words, when its
// own words don't say what to do.
export function addError(e, label) {
  if (e.code === 4) return `${label} wasn't added: this Outrigger doesn't take it yet. Update Outrigger from the home page, then try again.`;
  if (e.code === 5) return `${label} wasn't added: Outrigger has a different file under this version. It may have been re-released: remove the one on your board, apply, then add it again.`;
  return `${label} wasn't added. ${e.message}`;
}

export function removeError(e, label) {
  return `${label} wasn't removed. ${e.message}`;
}
