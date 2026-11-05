// The daily install email (workers/install-email/): what it says, when it
// stays quiet, and that it reads only the installs table and sends one email
// to the one address it's given.
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { reportDay, buildEmail, runDaily, NAMES } from "../../workers/install-email/summary.js";
import { fakeD1 } from "./d1.mjs";

const NOW = Date.parse("2026-11-06T15:00:00Z"); // the cron's hour; it reports 2026-11-05
const S = "waveshare-esp32-s3-touch-lcd-1.69";
const B = "waveshare-esp32-s3-touch-lcd-3.5";

const row = (day, board = S, failure = null, edition = "hub-loader") =>
  ({ day, board, edition, installer: "2026-11-04.2", result: failure ? "failed" : "ok", failure });

function db(rows) {
  const d = fakeD1();
  const insert = d.sqlite.prepare("INSERT INTO installs (day, board, edition, installer, result, failure) VALUES (?, ?, ?, ?, ?, ?)");
  for (const r of rows) insert.run(r.day, r.board, r.edition, r.installer, r.result, r.failure);
  return d;
}

function mailbox({ fails = false } = {}) {
  const sent = [];
  return { sent, send: async (m) => { if (fails) throw Object.assign(new Error("no"), { code: "E_SENDER_NOT_VERIFIED" }); sent.push(m); return { messageId: "m1" }; } };
}

const env = (prod, beta, email = mailbox()) =>
  ({ DB: db(prod), BETA: db(beta), EMAIL: email, TO: "owner@example.com", FROM: "counts@outrigger.music" });

test("it reports yesterday, in UTC, the day the installs table records", () => {
  assert.equal(reportDay(NOW), "2026-11-05");
  assert.equal(reportDay(Date.parse("2026-11-01T00:30:00Z")), "2026-10-31");
});

test("the email leads with yesterday, by board, then where failures stopped, then the totals", () => {
  const prod = [row("2026-11-04"), row("2026-11-05"), row("2026-11-05", B), row("2026-11-05", S, "read"),
    row("2026-11-05", S, "read"), row("2026-11-05", B, "write")];
  const { subject, text } = buildEmail({ prod, beta: [], day: "2026-11-05" });
  assert.equal(subject, "Outrigger installs on 2026-11-05: 2 finished, 3 failed");
  assert.match(text, /Waveshare 1\.69": 1 finished, 2 failed/);
  assert.match(text, /Waveshare 3\.5": 1 finished, 1 failed/);
  assert.match(text, /Where failures stopped: read 2, write 1/);
  assert.match(text, /Since the start: 3 finished, 3 failed/);
  assert.match(text, /once per board per browser/);
  assert.match(text, /UTC days/);
  assert.ok(text.indexOf("1.69") < text.indexOf("Since the start"));
});

test("no production installs yesterday means no email, whatever beta did", () => {
  assert.equal(buildEmail({ prod: [row("2026-11-04")], beta: [row("2026-11-05")], day: "2026-11-05" }), null);
});

test("beta gets one line of its own, only when it had installs yesterday", () => {
  const prod = [row("2026-11-05")];
  assert.doesNotMatch(buildEmail({ prod, beta: [row("2026-11-04")], day: "2026-11-05" }).text, /Beta/);
  assert.match(buildEmail({ prod, beta: [row("2026-11-05"), row("2026-11-05", S, "chip")], day: "2026-11-05" }).text,
    /Beta \(testing\): 1 finished, 1 failed/);
});

test("a board the email doesn't know by name shows its profile id", () => {
  assert.match(buildEmail({ prod: [row("2026-11-05", "some-new-board")], beta: [], day: "2026-11-05" }).text, /some-new-board/);
});

test("board names come from firmware/boards.json, so a new board can't be left out", () => {
  const table = JSON.parse(readFileSync(new URL("../../firmware/boards.json", import.meta.url), "utf8"));
  assert.deepEqual(NAMES, Object.fromEntries(Object.values(table.boards).map((b) => [b.site_id, b.short_name])));
});

test("runDaily sends one plain-text email to the configured address, from the configured sender", async () => {
  const e = env([row("2026-11-05")], []);
  assert.equal(await runDaily(e, NOW), "sent");
  assert.equal(e.EMAIL.sent.length, 1);
  const [m] = e.EMAIL.sent;
  assert.deepEqual(Object.keys(m).sort(), ["from", "subject", "text", "to"]);
  assert.equal(m.to, "owner@example.com");
  assert.deepEqual(m.from, { email: "counts@outrigger.music", name: "Outrigger" });
});

test("runDaily sends nothing on a quiet day", async () => {
  const e = env([row("2026-11-04")], []);
  assert.equal(await runDaily(e, NOW), "quiet");
  assert.equal(e.EMAIL.sent.length, 0);
});

test("beta's database failing doesn't stop production's email", async () => {
  const e = env([row("2026-11-05")], []);
  e.BETA = { prepare: () => { throw new Error("D1 down"); } };
  assert.equal(await runDaily(e, NOW), "sent");
  assert.doesNotMatch(e.EMAIL.sent[0].text, /Beta/);
});

test("a send that fails throws, so Cloudflare marks the run failed", async () => {
  await assert.rejects(runDaily(env([row("2026-11-05")], [], mailbox({ fails: true })), NOW), /no/);
});

test("without an address configured it refuses to send", async () => {
  const e = env([row("2026-11-05")], []);
  delete e.TO;
  await assert.rejects(runDaily(e, NOW), /TO/);
  assert.equal(e.EMAIL.sent.length, 0);
});
