// The Pages Function behind "Report this board" (functions/api/report.js),
// against D1's stand-in (d1.mjs): what it stores, what it refuses, and the
// rate limit, which must work without keeping anyone's IP address.
import { test } from "node:test";
import assert from "node:assert/strict";
import { handleReport, MAX_BODY, PER_HOUR, ALL_PER_HOUR } from "../../functions/api/report.js";
import { fakeD1 } from "./d1.mjs";

const IP = "203.0.113.7";
const NOW = Date.parse("2026-11-03T15:00:00Z");
const HOUR = 3600 * 1000;

const body = (extra = {}) => ({
  chip: "ESP32-S3 (QFN56) (revision v0.2)", flash_mb: 16, psram_mb: 8, usb_vid: 0x303a,
  picked: "Another ESP32-S3 board", verdict: "no profile yet", installer: "2026-11-03.1",
  board: "Waveshare ESP32-S3-Touch-LCD-2.8", note: "", email: "a@example.com", website: "", ...extra,
});

function post(data, { origin = "https://outrigger.music", ip = IP, raw } = {}) {
  const text = raw ?? JSON.stringify(data);
  // Cloudflare hands the Function the browser's Content-Length; Node's Request doesn't add one.
  const headers = { "content-type": "application/json", "cf-connecting-ip": ip,
    "content-length": String(new TextEncoder().encode(text).length) };
  if (origin) headers.origin = origin;
  return new Request("https://outrigger.music/api/report", { method: "POST", headers, body: text });
}

const send = (db, data, opts = {}) => handleReport(post(data, opts), db, opts.now ?? NOW);

test("a report is stored with its time, and the reply carries its number", async () => {
  const db = fakeD1();
  const res = await send(db, body());
  assert.equal(res.status, 200);
  const { ok, id } = await res.json();
  assert.equal(ok, true);
  const row = db.sqlite.prepare("SELECT * FROM reports WHERE id = ?").get(id);
  assert.equal(row.board, "Waveshare ESP32-S3-Touch-LCD-2.8");
  assert.equal(row.flash_mb, 16);
  assert.equal(row.usb_vid, 0x303a);
  assert.equal(row.email, "a@example.com");
  assert.equal(row.created_at, "2026-11-03T15:00:00.000Z");
});

test("an empty email is stored as no email", async () => {
  const db = fakeD1();
  const { id } = await (await send(db, body({ email: "" }))).json();
  assert.equal(db.sqlite.prepare("SELECT email FROM reports WHERE id = ?").get(id).email, null);
});

test("no IP address is stored anywhere", async () => {
  const db = fakeD1();
  await send(db, body());
  const tables = db.sqlite.prepare("SELECT name FROM sqlite_master WHERE type = 'table'").all().map((t) => t.name);
  for (const t of tables) {
    const dump = JSON.stringify(db.sqlite.prepare(`SELECT * FROM ${t}`).all());
    assert.ok(!dump.includes(IP), `${t}: ${dump}`);
  }
});

test("reports come only from outrigger.music, preview (and its old name, beta) and Pages previews", async () => {
  for (const origin of ["https://outrigger.music", "https://preview.outrigger.music", "https://beta.outrigger.music", "https://beta.outrigger.pages.dev",
    "https://3f2a1b.outrigger.pages.dev", "http://localhost:8788", "http://127.0.0.1:8788"]) {
    assert.equal((await send(fakeD1(), body(), { origin })).status, 200, origin);
  }
  for (const origin of ["https://evil.example", "https://outrigger.music.evil.example", "https://xoutrigger.music", "https://staging.outrigger.music",
    "https://outrigger.pages.dev.evil.example", null]) {
    assert.equal((await send(fakeD1(), body(), { origin })).status, 403, String(origin));
  }
});

test("a body over the cap is refused before it's read as JSON", async () => {
  const db = fakeD1();
  const res = await send(db, null, { raw: `{"board":"${"x".repeat(MAX_BODY)}"}` });
  assert.equal(res.status, 413);
  assert.equal(db.sqlite.prepare("SELECT count(*) AS n FROM reports").get().n, 0);
});

test("a body with no length is refused unread", async () => {
  const stream = new ReadableStream({ start(c) { c.enqueue(new TextEncoder().encode(JSON.stringify(body()))); c.close(); } });
  const req = new Request("https://outrigger.music/api/report", { method: "POST", body: stream, duplex: "half",
    headers: { origin: "https://outrigger.music", "cf-connecting-ip": IP } });
  assert.equal(req.headers.get("content-length"), null);
  assert.equal((await handleReport(req, fakeD1(), NOW)).status, 411);
});

test("not JSON, or a report that doesn't validate, is a 400 that says why", async () => {
  assert.equal((await send(fakeD1(), null, { raw: "{nope" })).status, 400);
  const res = await send(fakeD1(), body({ board: "" }));
  assert.equal(res.status, 400);
  assert.match((await res.json()).problems.join(" "), /board/i);
  assert.equal((await send(fakeD1(), body({ mac: "02:00:00:00:00:01" }))).status, 400);
});

test("a filled-in honeypot looks accepted but stores nothing", async () => {
  const db = fakeD1();
  const res = await send(db, body({ website: "http://spam.example" }));
  assert.equal(res.status, 200);
  assert.equal(db.sqlite.prepare("SELECT count(*) AS n FROM reports").get().n, 0);
});

test(`one visitor gets ${PER_HOUR} reports an hour, then a 429; an hour later, more`, async () => {
  const db = fakeD1();
  for (let i = 0; i < PER_HOUR; i++) assert.equal((await send(db, body(), { now: NOW + i })).status, 200, `report ${i + 1}`);
  assert.equal((await send(db, body(), { now: NOW + PER_HOUR })).status, 429);
  assert.equal((await send(db, body(), { ip: "198.51.100.1", now: NOW + PER_HOUR })).status, 200, "another visitor");
  assert.equal((await send(db, body(), { now: NOW + HOUR + PER_HOUR })).status, 200);
});

test(`everyone together gets ${ALL_PER_HOUR} reports an hour`, async () => {
  const db = fakeD1();
  for (let i = 0; i < ALL_PER_HOUR; i++) {
    assert.equal((await send(db, body(), { ip: `10.0.${i >> 8}.${i & 255}`, now: NOW + i })).status, 200);
  }
  assert.equal((await send(db, body(), { ip: "10.9.9.9", now: NOW + ALL_PER_HOUR })).status, 429);
});

test("rate-limit records and salts older than a day are deleted", async () => {
  const db = fakeD1();
  await send(db, body());
  await send(db, body(), { now: NOW + 25 * HOUR });
  const hits = db.sqlite.prepare("SELECT at FROM hits").all();
  assert.deepEqual(hits.map((h) => h.at), [NOW + 25 * HOUR]);
  assert.deepEqual(db.sqlite.prepare("SELECT day FROM salts").all().map((s) => s.day), ["2026-11-04"]);
});

test("the same visitor on a new day can't be linked to the day before", async () => {
  const db = fakeD1();
  await send(db, body());
  const before = db.sqlite.prepare("SELECT key FROM hits").get().key;
  await send(db, body(), { now: NOW + 24 * HOUR });
  const after = db.sqlite.prepare("SELECT key FROM hits").get().key;
  assert.notEqual(before, after);
});

test("a failed install's log is stored, with any MAC the page missed removed", async () => {
  const db = fakeD1();
  const log = "1.3 s  MAC: f4:12:fa:43:5b:c8\n1.9 s  Flash ID: 0x184020";
  const { id } = await (await send(db, body({ log, verdict: "The install stopped.", note: "F4-12-FA-43-5B-C8" }))).json();
  const row = db.sqlite.prepare("SELECT log, note FROM reports WHERE id = ?").get(id);
  assert.equal(row.log, "1.3 s  MAC: [MAC removed]\n1.9 s  Flash ID: 0x184020");
  assert.equal(row.note, "[MAC removed]");
  const dump = JSON.stringify(db.sqlite.prepare("SELECT * FROM reports").all()).toLowerCase();
  assert.ok(!dump.includes("f4:12") && !dump.includes("f4-12"), dump);
});

test("a board report without a log stores no log", async () => {
  const db = fakeD1();
  const { id } = await (await send(db, body())).json();
  assert.equal(db.sqlite.prepare("SELECT log FROM reports WHERE id = ?").get(id).log, null);
});
