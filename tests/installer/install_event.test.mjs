// The anonymous install count (site/installer/install_event.js): what the page
// sends when an install finishes or fails, what the page and the server both
// refuse, and when the page sends nothing.
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { buildInstallEvent, validateInstallEvent, sendInstallEvent, countingOn, setCounting,
  BOARDS, EDITIONS, FAILURES } from "../../site/installer/install_event.js";

const MAC = "f4:12:fa:43:5b:c8";
const BOARD = "waveshare-esp32-s3-touch-lcd-1.69";
const ok = (extra = {}) => buildInstallEvent({ board: BOARD, edition: "hub-loader", installer: "2026-11-04.2", ...extra });

// localStorage's calls, in memory; `broken` throws like a blocked or private window's.
function storage({ broken = false } = {}) {
  const items = new Map();
  const guard = () => { if (broken) throw new Error("SecurityError"); };
  return {
    items,
    getItem: (k) => (guard(), items.has(k) ? items.get(k) : null),
    setItem: (k, v) => (guard(), items.set(k, String(v))),
    removeItem: (k) => (guard(), items.delete(k)),
  };
}

function fakeFetch(status = 200) {
  const calls = [];
  const fn = async (url, init) => { calls.push({ url, init, body: JSON.parse(init.body) }); return { ok: status === 200, status }; };
  fn.calls = calls;
  return fn;
}

test("an event is exactly board, edition, installer, result and failure", () => {
  assert.deepEqual(ok(), { board: BOARD, edition: "hub-loader", installer: "2026-11-04.2", result: "ok", failure: null });
  assert.deepEqual(ok({ failure: "write" }),
    { board: BOARD, edition: "hub-loader", installer: "2026-11-04.2", result: "failed", failure: "write" });
  assert.equal(validateInstallEvent(ok()).ok, true);
  assert.equal(validateInstallEvent(ok({ failure: "chip" })).ok, true);
});

test("buildInstallEvent drops anything else it's handed, the MAC included", () => {
  const e = buildInstallEvent({ board: BOARD, edition: "hub", installer: "2026-11-04.2", mac: MAC, facts: { mac: MAC },
    log: [`MAC: ${MAC}`], userAgent: "Chrome" });
  assert.deepEqual(Object.keys(e).sort(), ["board", "edition", "failure", "installer", "result"]);
  assert.ok(!JSON.stringify(e).includes(MAC));
});

test("any field not on the list is refused", () => {
  for (const extra of [{ mac: MAC }, { ip: "203.0.113.7" }, { ua: "Chrome" }, { day: "2026-11-04" }, { website: "" }]) {
    const { ok: valid, problems } = validateInstallEvent({ ...ok(), ...extra });
    assert.equal(valid, false, JSON.stringify(extra));
    assert.match(problems.join(" "), /Unknown field/);
  }
});

test("a MAC, or any text, can't ride in a field: each one takes only its own fixed values", () => {
  const dashed = MAC.replaceAll(":", "-"); // a valid-looking slug, so the board list is fixed, not a pattern
  for (const bad of [{ board: MAC }, { board: dashed }, { board: `${BOARD} ${MAC}` }, { board: "" },
    { edition: MAC }, { edition: "hub " }, { installer: MAC }, { installer: "2026-11-04.2 f4" },
    { result: "ok!" }, { result: "failed", failure: MAC }, { result: "failed", failure: null },
    { result: "ok", failure: "write" }, { board: 7 }, { installer: null }]) {
    assert.equal(validateInstallEvent({ ...ok(), ...bad }).ok, false, JSON.stringify(bad));
  }
  assert.equal(validateInstallEvent(null).ok, false);
  assert.equal(validateInstallEvent([ok()]).ok, false);
});

test("the board list is every profile in firmware/boards.json, so a new board can't be left out", () => {
  const table = JSON.parse(readFileSync(new URL("../../firmware/boards.json", import.meta.url), "utf8"));
  assert.deepEqual([...BOARDS].sort(), Object.values(table.boards).map((b) => b.site_id).sort());
});

test("editions are the Makefile's app sets, failures the install's stages", () => {
  assert.deepEqual([...EDITIONS], ["hub", "hub-loader", "everything"]);
  assert.deepEqual([...FAILURES], ["download", "read", "chip", "store", "write"]);
});

test("a finished install is sent once, as JSON to /api/install, without cookies", async () => {
  const fetch = fakeFetch();
  const store = storage();
  assert.equal(await sendInstallEvent(ok(), { fetch, storage: store }), true);
  const [{ url, init, body }] = fetch.calls;
  assert.equal(url, "/api/install");
  assert.equal(init.method, "POST");
  assert.equal(init.credentials, "omit");
  assert.equal(init.keepalive, true);
  assert.deepEqual(body, ok());
});

test("a second finished install of the same board from this browser isn't counted again; another board is", async () => {
  const fetch = fakeFetch();
  const store = storage();
  await sendInstallEvent(ok(), { fetch, storage: store });
  assert.equal(await sendInstallEvent(ok(), { fetch, storage: store }), false);
  assert.equal(fetch.calls.length, 1);
  await sendInstallEvent(ok({ board: "waveshare-esp32-s3-touch-lcd-3.5" }), { fetch, storage: store });
  assert.equal(fetch.calls.length, 2);
});

test("failures are sent every time, and don't stop the success from counting later", async () => {
  const fetch = fakeFetch();
  const store = storage();
  await sendInstallEvent(ok({ failure: "read" }), { fetch, storage: store });
  await sendInstallEvent(ok({ failure: "read" }), { fetch, storage: store });
  await sendInstallEvent(ok(), { fetch, storage: store });
  assert.deepEqual(fetch.calls.map((c) => c.body.result), ["failed", "failed", "ok"]);
});

test("a success the server didn't take is tried again next install", async () => {
  const store = storage();
  const refused = fakeFetch(429);
  await sendInstallEvent(ok(), { fetch: refused, storage: store });
  const fetch = fakeFetch();
  assert.equal(await sendInstallEvent(ok(), { fetch, storage: store }), true);
  assert.equal(fetch.calls.length, 1);
});

test("unticking the box sends nothing, and stays unticked next visit", async () => {
  const fetch = fakeFetch();
  const store = storage();
  assert.equal(countingOn(store), true, "ticked by default");
  setCounting(store, false);
  assert.equal(countingOn(store), false);
  assert.equal(await sendInstallEvent(ok(), { fetch, storage: store }), false);
  assert.equal(await sendInstallEvent(ok({ failure: "write" }), { fetch, storage: store }), false);
  assert.equal(fetch.calls.length, 0);
  setCounting(store, true);
  assert.equal(countingOn(store), true);
});

test("an event that doesn't validate is never sent", async () => {
  const fetch = fakeFetch();
  assert.equal(await sendInstallEvent({ ...ok(), mac: MAC }, { fetch, storage: storage() }), false);
  assert.equal(fetch.calls.length, 0);
});

test("blocked storage still honours an unticked box and a counted board, for this page load", async () => {
  const off = storage({ broken: true });
  assert.equal(countingOn(off), true);
  setCounting(off, false);
  assert.equal(countingOn(off), false);
  assert.equal(await sendInstallEvent(ok(), { fetch: fakeFetch(), storage: off }), false);
  const store = storage({ broken: true });
  const fetch = fakeFetch();
  assert.equal(await sendInstallEvent(ok(), { fetch, storage: store }), true);
  assert.equal(await sendInstallEvent(ok(), { fetch, storage: store }), false);
  assert.equal(await sendInstallEvent(ok(), { fetch: fakeFetch(), storage: null }), true, "no storage at all");
});

test("a network error never throws", async () => {
  const down = async () => { throw new TypeError("Failed to fetch"); };
  assert.equal(await sendInstallEvent(ok(), { fetch: down, storage: storage() }), false);
});
