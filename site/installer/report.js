// "Report this board": what the page sends when the chip check finds a board
// with no profile, or one it refuses, and the rules the page and the server
// (functions/api/report.js) both check it against. Pure, so `make test` runs
// it on the Mac (tests/installer/report.test.mjs).
//
// The report is built from a fixed list of fields, never by copying esptool's
// facts: the chip's MAC is in those facts and in the log, and must not leave
// the page. The server refuses any field not listed here.
//
// "Send this log to Outrigger" (a failed install, report_form.js openLogForm)
// uses the same report with a log: esptool's progress lines only, never the
// serial trace (MAC bytes hide in its hex), with every MAC spelling replaced
// (scrubLog). The server scrubs again before storing.
import { chipFamily, flashBytesFromId, psramMbFromFeatures, USB_BRIDGES } from "./chip.js";

// Longest text each field may hold, in characters.
export const LIMITS = { chip: 200, picked: 100, verdict: 1500, installer: 40, board: 300, note: 4000, email: 254, log: 6000 };
// Their range; null when the chip didn't say. Whole numbers, except flash: some chips have 256 KB.
const NUMBERS = { flash_mb: 1024, psram_mb: 64, usb_vid: 0xffff };
// Left empty by people, filled in by form-filling bots (the page hides it).
const HONEYPOT = "website";
const FIELDS = new Set([...Object.keys(LIMITS), ...Object.keys(NUMBERS), HONEYPOT]);
const EMAIL = /^[^\s@]+@[^\s@]+\.[^\s@]+$/;

const text = (v) => String(v ?? "").trim();

// A MAC as colons, dashes, Cisco dots or 12 bare hex digits.
const MACS = /\b(?:[0-9a-f]{2}[:-]){5}[0-9a-f]{2}\b|\b[0-9a-f]{4}\.[0-9a-f]{4}\.[0-9a-f]{4}\b|\b[0-9a-f]{12}\b/gi;
export const scrubLog = (s) => String(s ?? "").replace(MACS, "[MAC removed]");

// A failed install's log report. log: area.js's progress lines (LOG), newest last; only the newest that fit go.
// error: the words the page showed; picked: the board chosen. The board field is the board picked.
export function buildLogReport({ log, error, picked, installer, note, email }) {
  const lines = [];
  let size = 0;
  for (const line of [...log].reverse().map(scrubLog)) {
    if (size + line.length + 1 > LIMITS.log) break;
    lines.unshift(line);
    size += line.length + 1;
  }
  return {
    chip: "", flash_mb: null, psram_mb: null, usb_vid: null,
    picked: text(picked).slice(0, LIMITS.picked), verdict: scrubLog(text(error)).slice(0, LIMITS.verdict),
    installer: text(installer), board: text(picked).slice(0, LIMITS.board) || "Not picked",
    note: scrubLog(text(note)), email: text(email), website: "", log: lines.join("\n"),
  };
}

// The chip has everything Outrigger needs (chip.js checkChip's bar), so the
// only thing missing is this board's profile. The pop-up says so.
export function boardFits(facts) {
  const flash = flashBytesFromId(facts.flashId);
  return chipFamily(facts.chip) === "ESP32-S3" && !USB_BRIDGES[facts.usbVendorId]
    && flash !== null && flash >= 16 * 1048576 && psramMbFromFeatures(facts.features) >= 8;
}

// facts: what area.js read ({chip, features, flashId, usbVendorId}); picked:
// the board chosen on the page; verdict: the chip check's words; the rest is
// what the person typed.
export function buildReport({ facts, picked, verdict, installer, board, note, email, website = "" }) {
  const flash = flashBytesFromId(facts.flashId);
  return {
    chip: text(facts.chip),
    flash_mb: flash === null ? null : flash / 1048576,
    psram_mb: psramMbFromFeatures(facts.features),
    usb_vid: Number.isInteger(facts.usbVendorId) ? facts.usbVendorId : null,
    picked: text(picked),
    verdict: text(verdict),
    installer: text(installer),
    board: text(board),
    note: text(note),
    email: text(email),
    website: text(website),
  };
}

// {ok, problems}: problems name the field, in words the page can show.
export function validateReport(r) {
  if (!r || typeof r !== "object" || Array.isArray(r)) return { ok: false, problems: ["The report isn't a form."] };
  const problems = [];
  for (const key of Object.keys(r)) if (!FIELDS.has(key)) problems.push(`Unknown field ${key}.`);
  for (const [key, max] of Object.entries(LIMITS)) {
    if (r[key] != null && typeof r[key] !== "string") problems.push(`${key} must be text.`);
    else if ((r[key] || "").length > max) problems.push(`${key} is longer than ${max} characters.`);
  }
  for (const [key, max] of Object.entries(NUMBERS)) {
    const v = r[key];
    const whole = key === "flash_mb" ? Number.isFinite(v) : Number.isInteger(v);
    if (v != null && !(whole && v >= 0 && v <= max)) problems.push(`${key} must be a number from 0 to ${max}.`);
  }
  if (typeof r.board !== "string" || !r.board.trim()) problems.push("Add the board's name or a link to it (board).");
  if (typeof r.email === "string" && r.email && !EMAIL.test(r.email)) problems.push("That email address doesn't look right (email).");
  return { ok: !problems.length, problems };
}
