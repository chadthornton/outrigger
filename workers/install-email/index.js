// outrigger-install-email: a Cloudflare Worker with no web address, woken by
// its cron (wrangler.toml) each morning to email the install counts
// (summary.js). Deployed with `make deploy-email`.
import { runDaily } from "./summary.js";

export default {
  async scheduled(event, env, ctx) {
    ctx.waitUntil(runDaily(env, event.scheduledTime).then((what) => console.log(`install email: ${what}`)));
  },
};
