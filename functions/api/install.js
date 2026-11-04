// POST /api/install: the installer's anonymous install count
// (site/installer/install_event.js, sent by area.js when an install finishes
// or fails) writes one row into D1's installs table (binding DB, as
// functions/api/report.js). tools/installs.py reads them.
//
// Stored: the day and the event's five fixed-list fields. Not stored: the
// time of day, the IP address (only report.js's salted, day-long rate-limit
// key), the user agent, anything else the request carries.
import { validateInstallEvent } from "../../site/installer/install_event.js";
import { ORIGIN, reply, limited } from "./report.js";

export const MAX_BODY = 1024;
// Generous: a launch day's installs must not be dropped. A person installs a few times an hour at most.
export const PER_HOUR = 20;
export const ALL_PER_HOUR = 2000;

// Testable without Cloudflare: request, a D1 database, and the time in ms.
export async function handleInstall(request, db, now) {
  if (!ORIGIN.test(request.headers.get("origin") || "")) return reply(403, { ok: false, problems: ["Counts come from outrigger.music."] });
  const length = request.headers.get("content-length");
  if (length === null) return reply(411, { ok: false, problems: ["The event has no length."] });
  if (Number(length) > MAX_BODY) return reply(413, { ok: false, problems: ["The event is too big."] });
  const raw = await request.text();
  if (raw.length > MAX_BODY) return reply(413, { ok: false, problems: ["The event is too big."] });
  let e;
  try {
    e = JSON.parse(raw);
  } catch {
    return reply(400, { ok: false, problems: ["The event isn't JSON."] });
  }
  const { ok, problems } = validateInstallEvent(e);
  if (!ok) return reply(400, { ok: false, problems });
  if (await limited(db, "install_hits", request.headers.get("cf-connecting-ip") || "", now, PER_HOUR, ALL_PER_HOUR)) {
    return reply(429, { ok: false, problems: ["Too many installs counted in the last hour."] });
  }
  await db.prepare("INSERT INTO installs (day, board, edition, installer, result, failure) VALUES (?, ?, ?, ?, ?, ?)")
    .bind(new Date(now).toISOString().slice(0, 10), e.board, e.edition, e.installer, e.result, e.failure).run();
  return reply(200, { ok: true });
}

export const onRequestPost = ({ request, env }) => handleInstall(request, env.DB, Date.now());
