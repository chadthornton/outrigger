// The "Report this board" form's payload (site/installer/report.js): what it
// takes from the chip check, and what the page and the server both refuse.
import { test } from "node:test";
import assert from "node:assert/strict";
import { buildReport, validateReport, boardFits, LIMITS } from "../../site/installer/report.js";

const MAC = "f4:12:fa:43:5b:c8";
// What area.js has after a refused chip check: esptool-js's facts, plus things
// that must never leave the page (the MAC, the log) riding along on purpose.
const facts = {
  chip: "ESP32-S3 (QFN56) (revision v0.2)",
  features: ["Wi-Fi", "BLE", "Embedded PSRAM 8MB (AP_3v3)"],
  flashId: 0x184020, // 16 MB
  usbVendorId: 0x303a,
  mac: MAC,
  macAddr: () => MAC,
  log: [`MAC: ${MAC}`],
};
const typed = { board: "Waveshare ESP32-S3-Touch-LCD-2.8", note: "Bought on Amazon", email: "a@example.com" };

const report = (extra = {}) => buildReport({ facts, picked: "Another ESP32-S3 board", verdict: "no profile yet",
  installer: "2026-11-03.1", ...typed, ...extra });

test("buildReport takes the chip, flash, PSRAM and USB vendor from what esptool read", () => {
  assert.deepEqual(report(), {
    chip: "ESP32-S3 (QFN56) (revision v0.2)", flash_mb: 16, psram_mb: 8, usb_vid: 0x303a,
    picked: "Another ESP32-S3 board", verdict: "no profile yet", installer: "2026-11-03.1",
    board: "Waveshare ESP32-S3-Touch-LCD-2.8", note: "Bought on Amazon", email: "a@example.com", website: "",
  });
});

test("buildReport never carries the MAC, whatever facts hold", () => {
  const sent = JSON.stringify(report());
  assert.ok(!sent.includes(MAC), sent);
  assert.ok(!/mac/i.test(Object.keys(report()).join(" ")));
});

test("buildReport: a flash chip that didn't answer is null, no PSRAM is 0", () => {
  const r = buildReport({ facts: { chip: "ESP32-S3", features: [], flashId: 0, usbVendorId: undefined }, picked: "x",
    verdict: "", installer: "v", board: "b", note: "", email: "" });
  assert.equal(r.flash_mb, null);
  assert.equal(r.psram_mb, 0);
  assert.equal(r.usb_vid, null);
});

test("buildReport trims what the person typed", () => {
  const r = report({ board: "  Freenove 2.8  ", email: " a@example.com \n" });
  assert.equal(r.board, "Freenove 2.8");
  assert.equal(r.email, "a@example.com");
});

test("validateReport passes a built report", () => {
  assert.deepEqual(validateReport(report()), { ok: true, problems: [] });
});

test("validateReport passes a report with no email and no note", () => {
  assert.equal(validateReport(report({ email: "", note: "" })).ok, true);
});

test("validateReport wants the board's name or a link", () => {
  const v = validateReport(report({ board: "   " }));
  assert.equal(v.ok, false);
  assert.match(v.problems.join(" "), /board/i);
});

test("validateReport refuses an email without an @ and a dot", () => {
  for (const email of ["nope", "a@b", "a b@c.d", "@c.de"]) {
    assert.equal(validateReport(report({ email })).ok, false, email);
  }
});

test("validateReport caps every text field", () => {
  for (const [field, max] of Object.entries(LIMITS)) {
    const over = field === "email" ? `${"a".repeat(max)}@example.com` : "x".repeat(max + 1);
    const v = validateReport({ ...report(), [field]: over });
    assert.equal(v.ok, false, field);
    assert.match(v.problems.join(" "), new RegExp(field), field);
  }
});

test("validateReport refuses fields it doesn't know, so nothing else gets stored", () => {
  const v = validateReport({ ...report(), mac: MAC });
  assert.equal(v.ok, false);
  assert.match(v.problems.join(" "), /mac/);
});

test("validateReport refuses numbers out of range and non-numbers", () => {
  for (const bad of [{ flash_mb: -1 }, { flash_mb: 4096 }, { psram_mb: "8" }, { usb_vid: 0x10000 }, { psram_mb: 1.5 }, { flash_mb: "16" }]) {
    assert.equal(validateReport({ ...report(), ...bad }).ok, false, JSON.stringify(bad));
  }
});

test("validateReport takes a flash chip under 1 MB", () => {
  assert.equal(validateReport({ ...report(), flash_mb: 0.25 }).ok, true);
});

test("validateReport refuses what isn't an object", () => {
  for (const bad of [null, [], "x", 3]) assert.equal(validateReport(bad).ok, false);
});

test("boardFits: an ESP32-S3 on its own USB with 16 MB flash and 8 MB PSRAM has what Outrigger needs", () => {
  assert.equal(boardFits(facts), true);
});

test("boardFits: anything short of that doesn't", () => {
  for (const [why, f] of [
    ["original ESP32", { ...facts, chip: "ESP32-D0WD-V3 (revision v3.1)" }],
    ["converter chip", { ...facts, usbVendorId: 0x1a86 }],
    ["8 MB flash", { ...facts, flashId: 0x174020 }],
    ["flash didn't answer", { ...facts, flashId: 0 }],
    ["2 MB PSRAM", { ...facts, features: ["Embedded PSRAM 2MB (AP_3v3)"] }],
    ["no PSRAM", { ...facts, features: ["Wi-Fi"] }],
  ]) assert.equal(boardFits(f), false, why);
});
