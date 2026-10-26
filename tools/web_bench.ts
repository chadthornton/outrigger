// Runs the routing latency bench on the hub and saves the results.
//
//   bun tools/web_bench.ts [host] [seconds] [out_dir]
//
// Needs the Launchkey's arpeggiator running (latched) for the whole run, so
// note gaps measure jitter. Modes, in order: wifi_off, idle, busy, then
// wifi_off again to check nothing drifted. During "busy" this script loads the
// hub the way a heavy browser session would: 4 WebSocket monitors plus 4
// loops fetching the page and the status JSON back to back.
import { mkdirSync, writeFileSync } from "node:fs";
import { pathToFileURL } from "node:url";

// Makes the calls do nothing where claude-progress is not installed.
const { claudeProgress } = await import(pathToFileURL(`${process.env.CLAUDE_PROGRESS_DIR}/claude-progress.js`).href).catch(() => ({ claudeProgress() {} }));

const host = process.argv[2] ?? "fm1hub.local";
const seconds = Number(process.argv[3] ?? 60);
const outDir = process.argv[4] ?? "build/bench";
const base = `http://${host}`;
// Bun sends the URL host as Host, so an IP works too only if we name the hub.
const headers = { Host: "fm1hub.local" };
const modes = process.env.MODES ? process.env.MODES.split(",") : ["wifi_off", "idle", "busy", "wifi_off"];
const sleep = (ms: number) => new Promise(r => setTimeout(r, ms));

async function reachable(): Promise<boolean> {
  try {
    const r = await fetch(`${base}/api/status`, { headers, signal: AbortSignal.timeout(2000) });
    return r.ok;
  } catch {
    return false;
  }
}

async function waitIdle(maxMs: number) {
  const until = Date.now() + maxMs;
  while (Date.now() < until) {
    try {
      const s = await (await fetch(`${base}/api/status`, { headers, signal: AbortSignal.timeout(2000) })).json();
      if (s.bench === "none") return;
    } catch {}
    await sleep(1000);
  }
  throw new Error("hub did not come back");
}

function load(stop: { done: boolean }) {
  const counts = { http: 0, ws: 0, errors: 0 };
  const sockets = [...Array(4)].map(() => {
    const ws = new WebSocket(`ws://${host}/ws`, { headers } as any);
    ws.onmessage = () => counts.ws++;
    ws.onerror = () => counts.errors++;
    return ws;
  });
  const loops = [...Array(4)].map(async (_, i) => {
    while (!stop.done) {
      try {
        const r = await fetch(`${base}${i % 2 ? "/api/status" : "/"}`, { headers, signal: AbortSignal.timeout(3000) });
        await r.arrayBuffer();
        counts.http++;
      } catch {
        counts.errors++;
      }
    }
  });
  return { counts, finish: async () => { await Promise.all(loops); sockets.forEach(s => s.close()); } };
}

if (!(await reachable())) throw new Error(`no hub at ${base}`);
mkdirSync(outDir, { recursive: true });
const loadStats: Record<string, unknown> = {};
const total = modes.length * (seconds + 15);
let elapsed = 0;

for (const [i, mode] of modes.entries()) {
  const r = await fetch(`${base}/api/bench/start?mode=${mode}&s=${seconds}`, { method: "POST", headers });
  if (!r.ok) throw new Error(`start ${mode}: ${await r.text()}`);
  console.log(`run ${i + 1}/${modes.length}: ${mode} for ${seconds} s`);
  const stop = { done: false };
  const busy = mode === "busy" ? load(stop) : null;
  const t0 = Date.now();
  while (Date.now() - t0 < (seconds + 3) * 1000) {
    await sleep(1000);
    claudeProgress("-n", "latency-bench", `${Math.min(total, elapsed + Math.round((Date.now() - t0) / 1000))}/${total}`, mode);
  }
  stop.done = true;
  if (busy) {
    await busy.finish();
    loadStats[`run${i + 1}_${mode}`] = { ...busy.counts, seconds };
    console.log(`  load: ${busy.counts.http} HTTP responses, ${busy.counts.ws} WS frames, ${busy.counts.errors} errors`);
  }
  await waitIdle(60000);
  // Keep each wifi_off run's results: the second overwrites the first on the hub.
  const results = await (await fetch(`${base}/api/bench`, { headers })).json();
  writeFileSync(`${outDir}/run${i + 1}_${mode}.json`, JSON.stringify({ ...results[mode], load: loadStats[`run${i + 1}_${mode}`] }, null, 2));
  for (const [kind, name] of [[0, "forward"], [1, "note_gaps"], [2, "clock_gaps"]] as const) {
    const raw = await (await fetch(`${base}/api/bench/raw?mode=${mode}&kind=${kind}`, { headers })).text();
    writeFileSync(`${outDir}/run${i + 1}_${mode}_${name}.json`, raw);
  }
  const f = results[mode]?.forward;
  const n = results[mode]?.note_gaps;
  console.log(`  forward us p50 ${f?.p50} p99 ${f?.p99} max ${f?.max} (n=${f?.n}); note gap jitter p99 ${n?.p99_dev} max ${n?.max_dev} (n=${n?.n}); failures ${results[mode]?.send_failures}`);
  elapsed += seconds + 15;
}
claudeProgress("-n", "latency-bench", "done");
console.log(`saved to ${outDir}`);
