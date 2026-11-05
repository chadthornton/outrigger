// "Send this log to Outrigger" (site/installer/report.js buildLogReport,
// scrubLog): what a failed install's log report carries, and that the chip's
// MAC never leaves the page in any spelling.
import { test } from "node:test";
import assert from "node:assert/strict";
import { buildLogReport, scrubLog, validateReport, LIMITS } from "../../site/installer/report.js";

const MAC = "f4:12:fa:43:5b:c8";
const SPELLINGS = [MAC, MAC.toUpperCase(), MAC.replaceAll(":", "-"), MAC.replaceAll(":", ""), "f412.fa43.5bc8",
  MAC.replaceAll(":", "").toUpperCase()];
const LOG = ["0.4 s  Connecting...", "1.2 s  Chip is ESP32-S3 (QFN56) (revision v0.2)",
  "1.3 s  Features: Wi-Fi, BLE, Embedded PSRAM 8MB (AP_3v3)", `1.3 s  MAC: ${MAC}`, "1.9 s  Flash ID: 0x184020",
  "88.0 s  Writing at 0x00120000... (42 %)"];

const report = (extra = {}) => buildLogReport({ log: LOG, error: "The install stopped. (Timeout)", picked: "Waveshare 1.69\"",
  installer: "2026-11-05.2", note: "", email: "", ...extra });

test("scrubLog removes a MAC in every spelling, and leaves the rest", () => {
  for (const mac of SPELLINGS) {
    const out = scrubLog(`MAC: ${mac} then more`);
    assert.equal(out, "MAC: [MAC removed] then more", mac);
  }
  assert.equal(scrubLog("Flash ID: 0x184020, Writing at 0x00120000"), "Flash ID: 0x184020, Writing at 0x00120000");
});

test("a log report carries the log lines, the error, the board picked and the installer, and no MAC", () => {
  const r = report();
  assert.equal(r.board, "Waveshare 1.69\"");
  assert.equal(r.picked, "Waveshare 1.69\"");
  assert.equal(r.verdict, "The install stopped. (Timeout)");
  assert.equal(r.installer, "2026-11-05.2");
  assert.match(r.log, /Chip is ESP32-S3/);
  assert.match(r.log, /Writing at 0x00120000/);
  for (const mac of SPELLINGS) assert.ok(!JSON.stringify(r).toLowerCase().includes(mac.toLowerCase()), mac);
  assert.ok(validateReport(r).ok, validateReport(r).problems.join(" "));
});

test("a MAC typed into the note or the error is removed too", () => {
  const r = report({ note: `my board ${MAC}`, error: `failed on ${MAC.replaceAll(":", "-")}` });
  assert.ok(!JSON.stringify(r).includes("43:5b") && !JSON.stringify(r).includes("43-5b"));
});

test("only the last lines go, within the log's limit", () => {
  const long = Array.from({ length: 500 }, (_, i) => `${i}.0 s  ${"x".repeat(60)} line ${i}`);
  const r = report({ log: long });
  assert.ok(r.log.length <= LIMITS.log);
  assert.match(r.log, /line 499$/);
  assert.ok(validateReport(r).ok);
});

test("validateReport caps the log and wants it to be text", () => {
  assert.equal(validateReport({ ...report(), log: "x".repeat(LIMITS.log + 1) }).ok, false);
  assert.equal(validateReport({ ...report(), log: ["a"] }).ok, false);
});
