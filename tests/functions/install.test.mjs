// The Pages Function behind the anonymous install count (functions/api/install.js),
// against D1's stand-in (d1.mjs): what it stores (no time of day, no IP), what it
// refuses, and its rate limit, kept apart from the report form's.
import { test } from "node:test";
import assert from "node:assert/strict";
import { handleInstall, MAX_BODY, PER_HOUR, ALL_PER_HOUR } from "../../functions/api/install.js";
import { handleReport, PER_HOUR as REPORTS_PER_HOUR } from "../../functions/api/report.js";
import { fakeD1 } from "./d1.mjs";

const IP = "203.0.113.7";
const MAC = "f4:12:fa:43:5b:c8";
const NOW = Date.parse("2026-11-04T15:42:07Z");
const HOUR = 3600 * 1000;

const event = (extra = {}) => ({ board: "waveshare-esp32-s3-touch-lcd-1.69", edition: "hub-loader",
  installer: "2026-11-04.2", result: "ok", failure: null, ...extra });

function post(data, { origin = "https://outrigger.music", ip = IP, raw, path = "/api/install" } = {}) {
  const text = raw ?? JSON.stringify(data);
  const headers = { "content-type": "application/json", "cf-connecting-ip": ip, "user-agent": "Mozilla/5.0 Chrome/130",
    "content-length": String(new TextEncoder().encode(text).length) };
  if (origin) headers.origin = origin;
  return new Request(`https://outrigger.music${path}`, { method: "POST", headers, body: text });
}

const send = (db, data, opts = {}) => handleInstall(post(data, opts), db, opts.now ?? NOW);
const rows = (db) => db.sqlite.prepare("SELECT * FROM installs").all();

test("an install is stored with the day only, and its five fields", async () => {
  const db = fakeD1();
  assert.equal((await send(db, event())).status, 200);
  assert.equal((await send(db, event({ result: "failed", failure: "write" }))).status, 200);
  const [okRow, failed] = rows(db);
  const { id, ...rest } = okRow;
  assert.ok(id);
  assert.deepEqual(rest, { day: "2026-11-04", board: "waveshare-esp32-s3-touch-lcd-1.69", edition: "hub-loader",
    installer: "2026-11-04.2", result: "ok", failure: null });
  assert.equal(failed.failure, "write");
});

test("no IP address, user agent or time of day is stored anywhere", async () => {
  const db = fakeD1();
  await send(db, event());
  const tables = db.sqlite.prepare("SELECT name FROM sqlite_master WHERE type = 'table'").all().map((t) => t.name);
  for (const t of tables) {
    const dump = JSON.stringify(db.sqlite.prepare(`SELECT * FROM ${t}`).all());
    assert.ok(!dump.includes(IP) && !dump.includes("Chrome") && !dump.includes("15:42"), `${t}: ${dump}`);
  }
  assert.deepEqual(db.sqlite.prepare("PRAGMA table_info(installs)").all().map((c) => c.name),
    ["id", "day", "board", "edition", "installer", "result", "failure"]);
});

test("a MAC, an extra field, or a field outside its fixed values is a 400 that stores nothing", async () => {
  const db = fakeD1();
  for (const bad of [event({ mac: MAC }), event({ board: MAC }), event({ board: MAC.replaceAll(":", "-") }),
    event({ edition: "hub", note: "hi" }), event({ result: "failed", failure: MAC }), event({ day: "2026-01-01" })]) {
    assert.equal((await send(db, bad)).status, 400, JSON.stringify(bad));
  }
  assert.equal((await send(db, null, { raw: "{nope" })).status, 400);
  assert.equal(rows(db).length, 0);
});

test("installs come only from outrigger.music, preview (and its old name, beta) and Pages previews", async () => {
  for (const origin of ["https://outrigger.music", "https://preview.outrigger.music", "https://beta.outrigger.music", "https://3f2a1b.outrigger.pages.dev",
    "http://localhost:8788"]) {
    assert.equal((await send(fakeD1(), event(), { origin })).status, 200, origin);
  }
  for (const origin of ["https://evil.example", "https://outrigger.music.evil.example", null]) {
    assert.equal((await send(fakeD1(), event(), { origin })).status, 403, String(origin));
  }
});

test("a body over the cap, or with no length, is refused unread", async () => {
  const db = fakeD1();
  assert.equal((await send(db, null, { raw: `{"board":"${"x".repeat(MAX_BODY)}"}` })).status, 413);
  const stream = new ReadableStream({ start(c) { c.enqueue(new TextEncoder().encode(JSON.stringify(event()))); c.close(); } });
  const req = new Request("https://outrigger.music/api/install", { method: "POST", body: stream, duplex: "half",
    headers: { origin: "https://outrigger.music", "cf-connecting-ip": IP } });
  assert.equal((await handleInstall(req, db, NOW)).status, 411);
  assert.equal(rows(db).length, 0);
});

test(`one visitor gets ${PER_HOUR} an hour, everyone ${ALL_PER_HOUR}; more are a 429 that stores nothing`, async () => {
  const db = fakeD1();
  for (let i = 0; i < PER_HOUR; i++) assert.equal((await send(db, event(), { now: NOW + i })).status, 200);
  assert.equal((await send(db, event(), { now: NOW + PER_HOUR })).status, 429);
  assert.equal((await send(db, event(), { now: NOW + HOUR + PER_HOUR })).status, 200, "an hour later");
  const all = fakeD1();
  for (let i = 0; i < ALL_PER_HOUR; i++) await send(all, event(), { ip: `10.${i >> 16}.${(i >> 8) & 255}.${i & 255}`, now: NOW + i });
  assert.equal((await send(all, event(), { ip: "10.250.9.9", now: NOW + ALL_PER_HOUR })).status, 429);
  assert.equal(rows(all).length, ALL_PER_HOUR);
});

test("install pings don't use up the report form's limit", async () => {
  const db = fakeD1();
  for (let i = 0; i < PER_HOUR; i++) await send(db, event(), { now: NOW + i });
  const report = { board: "Some board", installer: "2026-11-04.2" };
  for (let i = 0; i < REPORTS_PER_HOUR; i++) {
    assert.equal((await handleReport(post(report, { path: "/api/report" }), db, NOW + PER_HOUR + i)).status, 200, `report ${i + 1}`);
  }
});

test("rate-limit records older than a day are deleted, and a new day's key can't be linked to the last", async () => {
  const db = fakeD1();
  await send(db, event());
  const before = db.sqlite.prepare("SELECT key FROM install_hits").get().key;
  await send(db, event(), { now: NOW + 25 * HOUR });
  const hits = db.sqlite.prepare("SELECT key, at FROM install_hits").all();
  assert.deepEqual(hits.map((h) => h.at), [NOW + 25 * HOUR]);
  assert.notEqual(hits[0].key, before);
  assert.deepEqual(db.sqlite.prepare("SELECT day FROM salts").all().map((s) => s.day), ["2026-11-05"]);
});
