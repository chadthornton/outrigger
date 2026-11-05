// POST /api/report: the installer's "Report this board" form
// (site/installer/report_form.js) writes one row into D1 (binding DB:
// outrigger-reports on outrigger.music, outrigger-reports-beta on preview.outrigger.music
// and the other previews; wrangler.toml). tools/reports.py reads them.
//
// Rate limit without keeping IP addresses: each visitor is a SHA-256 of
// today's random salt and their address. The salt lives in D1 for the day it
// was made and is deleted with the day's records, after which a hash can't be
// tied to an address, or to the same visitor's hash the next day.
import { validateReport, scrubLog } from "../../site/installer/report.js";

export const MAX_BODY = 16 * 1024;
export const PER_HOUR = 5;
export const ALL_PER_HOUR = 60;
const HOUR = 3600 * 1000;
const DAY = 24 * HOUR;

// outrigger.music, preview (and beta, its old name, which redirects there), Pages previews
// (<hash>.outrigger.pages.dev), and wrangler pages dev on the Mac.
export const ORIGIN = /^(https:\/\/((preview\.|beta\.)?outrigger\.music|([a-z0-9-]+\.)?outrigger\.pages\.dev)|http:\/\/(localhost|127\.0\.0\.1)(:\d+)?)$/;

export const reply = (status, data) => new Response(JSON.stringify(data), {
  status, headers: { "content-type": "application/json", "cache-control": "no-store" },
});

const hex = (buf) => [...new Uint8Array(buf)].map((b) => b.toString(16).padStart(2, "0")).join("");

export async function visitor(db, ip, now) {
  const day = new Date(now).toISOString().slice(0, 10);
  await db.prepare("DELETE FROM salts WHERE day <> ?").bind(day).run();
  await db.prepare("INSERT OR IGNORE INTO salts (day, salt) VALUES (?, ?)")
    .bind(day, hex(crypto.getRandomValues(new Uint8Array(32)))).run();
  const { salt } = await db.prepare("SELECT salt FROM salts WHERE day = ?").bind(day).first();
  return hex(await crypto.subtle.digest("SHA-256", new TextEncoder().encode(`${salt}|${ip}`)));
}

// true when this visitor, or everyone together, has had their share this hour; otherwise counts this one. table:
// "hits" (reports) or "install_hits" (installs, functions/api/install.js), so each keeps its own limit. Rows older
// than a day are deleted.
export async function limited(db, table, ip, now, perHour, allPerHour) {
  if (!["hits", "install_hits"].includes(table)) throw new Error(`no rate-limit table ${table}`);
  await db.prepare(`DELETE FROM ${table} WHERE at <= ?`).bind(now - DAY).run();
  const key = await visitor(db, ip, now);
  const mine = await db.prepare(`SELECT count(*) AS n FROM ${table} WHERE key = ? AND at > ?`).bind(key, now - HOUR).first();
  const all = await db.prepare(`SELECT count(*) AS n FROM ${table} WHERE at > ?`).bind(now - HOUR).first();
  if (mine.n >= perHour || all.n >= allPerHour) return true;
  await db.prepare(`INSERT INTO ${table} (key, at) VALUES (?, ?)`).bind(key, now).run();
  return false;
}

// Testable without Cloudflare: request, a D1 database, and the time in ms.
export async function handleReport(request, db, now) {
  if (!ORIGIN.test(request.headers.get("origin") || "")) return reply(403, { ok: false, problems: ["Reports come from outrigger.music."] });
  // The page's fetch always sends a length; without one the body could be any size before it's read.
  const length = request.headers.get("content-length");
  if (length === null) return reply(411, { ok: false, problems: ["The report has no length."] });
  if (Number(length) > MAX_BODY) return reply(413, { ok: false, problems: ["The report is too big."] });
  const raw = await request.text();
  if (raw.length > MAX_BODY) return reply(413, { ok: false, problems: ["The report is too big."] });
  let r;
  try {
    r = JSON.parse(raw);
  } catch {
    return reply(400, { ok: false, problems: ["The report isn't JSON."] });
  }
  const { ok, problems } = validateReport(r);
  if (!ok) return reply(400, { ok: false, problems });
  if (r.website) return reply(200, { ok: true }); // a bot filled in the hidden field

  if (await limited(db, "hits", request.headers.get("cf-connecting-ip") || "", now, PER_HOUR, ALL_PER_HOUR)) {
    return reply(429, { ok: false, problems: ["Too many reports in the last hour. Try again later."] });
  }

  const row = await db.prepare(
    "INSERT INTO reports (created_at, chip, flash_mb, psram_mb, usb_vid, picked, verdict, installer, board, note, email, log) " +
    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
  ).bind(new Date(now).toISOString(), r.chip || null, r.flash_mb ?? null, r.psram_mb ?? null, r.usb_vid ?? null,
    r.picked || null, r.verdict ? scrubLog(r.verdict) : null, r.installer || null, r.board.trim(), r.note ? scrubLog(r.note) : null,
    r.email || null, r.log ? scrubLog(r.log) : null).run(); // scrubbed again: the page's scrub is the first line, not the only one
  return reply(200, { ok: true, id: row.meta.last_row_id });
}

export const onRequestPost = ({ request, env }) => handleReport(request, env.DB, Date.now());
