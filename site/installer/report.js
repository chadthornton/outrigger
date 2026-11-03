// "Report this board": what the page sends when the chip check finds a board
// with no profile, or one it refuses, and the rules the page and the server
// (functions/api/report.js) both check it against. Pure, so `make test` runs
// it on the Mac (tests/installer/report.test.mjs).
//
// The report is built from a fixed list of fields, never by copying esptool's
// facts: the chip's MAC is in those facts and in the log, and must not leave
// the page. The server refuses any field not listed here.
import { flashBytesFromId, psramMbFromFeatures } from "./chip.js";

// Longest text each field may hold, in characters.
export const LIMITS = { chip: 200, picked: 100, verdict: 1500, installer: 40, board: 300, note: 4000, email: 254 };
// Their range; null when the chip didn't say. Whole numbers, except flash: some chips have 256 KB.
const NUMBERS = { flash_mb: 1024, psram_mb: 64, usb_vid: 0xffff };
// Left empty by people, filled in by form-filling bots (the page hides it).
const HONEYPOT = "website";
const FIELDS = new Set([...Object.keys(LIMITS), ...Object.keys(NUMBERS), HONEYPOT]);
const EMAIL = /^[^\s@]+@[^\s@]+\.[^\s@]+$/;

const text = (v) => String(v ?? "").trim();

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
