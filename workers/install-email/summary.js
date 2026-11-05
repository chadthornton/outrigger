// The daily install email: what it says and when it stays quiet. Pure apart
// from the env it's handed, so `make test` runs it on the Mac
// (tests/functions/install_email.test.mjs). index.js runs it on Cloudflare's
// cron, against the installs tables functions/api/install.js writes.
//
// It reports yesterday (UTC, the day the table records), and sends nothing on a
// day production had no installs. One recipient, env.TO, a Worker secret: the
// address never sits in this public repo, and Cloudflare only delivers to
// addresses confirmed in the account.

// firmware/boards.json's site_id -> short_name (a test keeps them equal).
export const NAMES = {
  "waveshare-esp32-s3-touch-lcd-1.69": "Waveshare 1.69\"",
  "waveshare-esp32-s3-touch-lcd-3.5": "Waveshare 3.5\"",
  "freenove-esp32-s3-display-2.8": "Freenove 2.8\"",
};
const DAY = 24 * 3600 * 1000;
const SQL = "SELECT day, board, result, failure FROM installs";

export const reportDay = (now) => new Date(now - DAY).toISOString().slice(0, 10);

const tally = (rows) => [rows.filter((r) => r.result === "ok").length, rows.filter((r) => r.result !== "ok").length];
const pair = ([ok, failed]) => `${ok} finished, ${failed} failed`;

function byBoard(rows) {
  const boards = new Map();
  for (const r of rows) boards.set(r.board, [...(boards.get(r.board) || []), r]);
  return [...boards].sort((a, b) => b[1].length - a[1].length).map(([board, rs]) => `  ${NAMES[board] || board}: ${pair(tally(rs))}`);
}

function failures(rows) {
  const counts = new Map();
  for (const r of rows) if (r.result !== "ok") counts.set(r.failure, (counts.get(r.failure) || 0) + 1);
  return [...counts].sort((a, b) => b[1] - a[1]).map(([k, n]) => `${k} ${n}`).join(", ");
}

// prod, beta: every row of each database's installs table. {subject, text}, or null on a quiet day.
export function buildEmail({ prod, beta, day }) {
  const today = prod.filter((r) => r.day === day);
  if (!today.length) return null;
  const betaToday = beta.filter((r) => r.day === day);
  const stopped = failures(today);
  const lines = [`Installs on outrigger.music on ${day}:`, ...byBoard(today), ""];
  if (stopped) lines.push(`Where failures stopped: ${stopped}`, "");
  lines.push(`Since the start: ${pair(tally(prod))}`, ...byBoard(prod), "");
  if (betaToday.length) lines.push(`Beta (testing): ${pair(tally(betaToday))}`, "");
  lines.push("A finished install counts once per board per browser; a failure counts every attempt, so failures read high.",
    "Sent by the outrigger-install-email Worker (workers/install-email/ in the repo).");
  return { subject: `Outrigger installs on ${day}: ${pair(tally(today))}`, text: lines.join("\n") };
}

async function rows(db) {
  return (await db.prepare(SQL).all()).results;
}

// "sent" or "quiet". Throws when the send fails, so the cron run shows as failed in Cloudflare.
export async function runDaily(env, now) {
  if (!env.TO) throw new Error("No recipient: set the TO secret (make deploy-email).");
  const prod = await rows(env.DB);
  let beta = [];
  try {
    beta = await rows(env.BETA);
  } catch {
    // beta's count is a footnote; production's email still goes
  }
  const email = buildEmail({ prod, beta, day: reportDay(now) });
  if (!email) return "quiet";
  await env.EMAIL.send({ to: env.TO, from: { email: env.FROM, name: "Outrigger" }, subject: email.subject, text: email.text });
  return "sent";
}
