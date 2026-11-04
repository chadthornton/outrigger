// The anonymous install count: what the page sends when an install it started
// finishes or fails, the rules the page and the server (functions/api/install.js)
// both check it against, and when the page sends nothing. Pure apart from the
// fetch and storage it's handed, so `make test` runs it on the Mac
// (tests/installer/install_event.test.mjs).
//
// An event is five fields, each from a fixed list or pattern, so nothing else
// (the chip's MAC, the log, the browser) can ride along. The server adds the
// day and nothing more: no time, no IP, no user agent, no cookies.
//
// Counting rule: a finished install counts once per board profile per
// browser (a localStorage flag, set when the server took it), so reinstalling
// the same board doesn't count again; a second kind of board does. Failures
// count every time. Unticking "Count this install" sends nothing and stays
// unticked in this browser.

// firmware/boards.json's site_ids (a test keeps them equal). A fixed list, not a pattern: a MAC written with
// dashes is a valid-looking slug.
export const BOARDS = new Set(["waveshare-esp32-s3-touch-lcd-1.69", "waveshare-esp32-s3-touch-lcd-3.5",
  "freenove-esp32-s3-display-2.8"]);
// The Makefile's app sets (tools/build_site.py APP_SETS).
export const EDITIONS = new Set(["hub", "hub-loader", "everything"]);
// Where an install stopped (site/installer/area.js picks one per step, never from an error's words).
export const FAILURES = new Set(["download", "read", "chip", "store", "write"]);
const RESULTS = new Set(["ok", "failed"]);
const INSTALLER = /^\d{4}-\d{2}-\d{2}\.\d{1,3}$/;
const FIELDS = ["board", "edition", "installer", "result", "failure"];

export const ENDPOINT = "/api/install";
const OFF = "outrigger-count-off";
const counted = (board) => `outrigger-counted-${board}`;

// failure: one of FAILURES, or none for a finished install.
export function buildInstallEvent({ board, edition, installer, failure = null }) {
  return { board, edition, installer, result: failure ? "failed" : "ok", failure: failure || null };
}

// {ok, problems}, the same words on the page and the server.
export function validateInstallEvent(e) {
  if (!e || typeof e !== "object" || Array.isArray(e)) return { ok: false, problems: ["The event isn't an object."] };
  const problems = [];
  for (const key of Object.keys(e)) if (!FIELDS.includes(key)) problems.push(`Unknown field ${key}.`);
  if (!BOARDS.has(e.board)) problems.push("board isn't a known profile.");
  if (!EDITIONS.has(e.edition)) problems.push("edition isn't a known app set.");
  if (typeof e.installer !== "string" || !INSTALLER.test(e.installer)) problems.push("installer isn't a version.");
  if (!RESULTS.has(e.result)) problems.push("result must be ok or failed.");
  else if (e.result === "ok" ? e.failure !== null : !FAILURES.has(e.failure)) problems.push("failure doesn't match result.");
  return { ok: !problems.length, problems };
}

// Storage can be missing or throw (blocked site data, private windows). Then this page load remembers instead, so
// an unticked box stays unticked and a counted board isn't counted again until the page reloads.
const memory = new Map(); // storage -> {key: value}, only for a storage that throws
const remembered = (storage) => memory.get(storage) || memory.set(storage, {}).get(storage);

function read(storage, key) {
  try {
    return storage.getItem(key);
  } catch {
    return remembered(storage)[key] ?? null;
  }
}

function write(storage, key, value) {
  try {
    if (value === null) storage.removeItem(key);
    else storage.setItem(key, value);
  } catch {
    if (value === null) delete remembered(storage)[key];
    else remembered(storage)[key] = value;
  }
}

export const countingOn = (storage) => read(storage, OFF) !== "1";
export const setCounting = (storage, on) => write(storage, OFF, on ? null : "1");

// true when the server took it. Never throws: the count must not get in the way of an install.
export async function sendInstallEvent(event, { fetch, storage }) {
  if (!countingOn(storage) || !validateInstallEvent(event).ok) return false;
  if (event.result === "ok" && read(storage, counted(event.board))) return false;
  try {
    const res = await fetch(ENDPOINT, { method: "POST", keepalive: true, credentials: "omit",
      headers: { "content-type": "application/json" }, body: JSON.stringify(event) });
    if (!res.ok) return false;
  } catch {
    return false;
  }
  if (event.result === "ok") write(storage, counted(event.board), "1");
  return true;
}
