# Publishing the site

The site is on Cloudflare Pages (project `outrigger`, a Direct Upload project in Chad's account):

| Channel | URL | Deploys |
|---|---|---|
| production | https://outrigger.music | `origin/master`'s commit only |
| beta | https://beta.outrigger.music | any pushed commit |

```
make deploy-site                     # beta
make deploy-site CHANNEL=production  # outrigger.music
make deploy-site FULL=1              # build it with make check on the Mac instead of taking CI's
```

- **What gets uploaded:** usually CI's build of that exact commit, in about a minute. A push to `master` makes CI build
  every board × app set and keep it as the `site-firmware` artifact (14 days) with a `SHA256SUMS` list.
  `make deploy-site` takes it only from a CI run of exactly HEAD with `ci-ok` green, and checks it three ways: the
  download against GitHub's digest, every file against `SHA256SUMS`, and every manifest against HEAD (clean build, parts
  matching). The rest comes from git at that commit: the pages, `functions/`, `wrangler.toml`, `NOTICE` and `LICENSE`.
  It runs `make check` on the Mac and uploads that build (`build/checked/site`) instead in three cases:
  - `FULL=1`;
  - a batch whose changes since the commit production serves touch `firmware/`, `lib/` or the critical list's code
    (`CRITICAL` in `tools/deploy_site.py`), on beta too, so beta carries the same build production will;
  - a commit CI never built by itself: any branch commit, because CI builds a PR merged into `master`. Start CI on the
    branch by hand (`gh workflow run CI --ref <branch>`) to get a build of the commit itself.

  A red or still-running CI run refuses either way, and production refuses until CI has run.
- **Production doesn't build again:** it uploads beta's kept copy of the same commit (the exact bytes checked on beta,
  verified against their SHA-256 sums), so a firmware ship runs `make check` once, for beta. It builds afresh only for
  `FULL=1`, when beta's copy is missing or changed, or when beta took CI's build of a batch that needs the Mac's.
- **CI's build vs the Mac's:** the app binaries differ byte for byte (each embeds its own toolchain paths, e.g.
  `/home/runner/.arduino15/…`); bootloaders, partition
  tables, pages and Functions are identical (compared at d5bc202, 2026-11-03). Nothing uncommitted or unpushed goes
  live. When production may go up without Chad: [CONTRIBUTING.md](../CONTRIBUTING.md), "Shipping to production".
- **What gets checked first:** `tools/deploy_site.py` refuses a dirty tree, an unpushed HEAD, or production from
  anything but `origin/master` or from a commit beta doesn't serve. Before uploading it scans every file again: under Pages' 25 MiB per-file and
  20,000-file limits, and free of `wl82loader.bin` (by name, SHA-256 and 1 KiB pieces). A local copy of the blob is
  required for that scan.
- **Which commit is live:** each upload carries its commit hash, shown in the Pages dashboard under Workers & Pages,
  then `outrigger`, then Deployments. Every deploy and rollback is also a line in the deploy log,
  `~/Library/Application Support/outrigger/deploys/deploys.jsonl` (commit, Pages deployment ID, time, session), which
  every worktree on the Mac shares. `tail -1` of it, filtered by channel, is what that channel serves.
- **Rollback:** `make rollback-site CHANNEL=production` (or `beta`) puts back the deploy before the live one, in
  seconds and without `make check`; `COMMIT=<sha>` goes back further. Production uses Cloudflare's own rollback (about
  a second, no upload; the old Functions come back with the old site). Cloudflare can't roll back a preview, so for
  beta each deploy keeps what it uploaded (newest 3, beside the log, with SHA-256 sums) and a rollback uploads that copy
  again. A second rollback goes further back, never forward to the build the first one left. It refuses when the log
  has no earlier deploy, when the commit isn't one the log can go back to, and when the channel serves something the
  log didn't deploy (use the dashboard's "Rollback to this deployment" then). Any agent may roll production back the
  moment it's broken, then tell Chad.
- **Board reports:** the installer's "Report this board" pop-up (`site/installer/report_form.js`) posts to a Pages
  Function, `functions/api/report.js`, which writes to D1: `outrigger-reports` for production, `outrigger-reports-beta`
  for beta and previews (`wrangler.toml`, schema in `migrations/`). The function and `wrangler.toml` go up with the
  site, from the same checked commit (`build/checked/`). Read new reports with `python3 -I tools/reports.py new`
  (add `beta` for beta's); `forget <id>` deletes a report's email when someone asks.
- **Failure logs:** a failed install offers "Send this log to Outrigger" (`site/installer/report_form.js` openLogForm).
  It's a report with a `log` field: esptool's progress lines only, never the serial trace, shown to the visitor in
  full before sending. Every MAC spelling is replaced by the page (`report.js` scrubLog) and again by
  `functions/api/report.js`. `tools/reports.py new` prints the log under the report.
- **Install counts:** when an install finishes or fails after the visitor pressed Install, `site/installer/area.js` posts an
  anonymous event (`site/installer/install_event.js`: board profile, edition, installer version, result and where it
  stopped) to `functions/api/install.js`, which stores it with the day only, in the same databases' `installs` table.
  No MAC, serial, IP, user agent or cookie. A finish counts once per board per browser (a localStorage flag);
  failures count every attempt. The box sits only beside the Install button (nothing is counted before that click); unticking it
  is remembered in that browser. The FAQ entry "Does this site send anything about me?" says the same, so change both together.
  `python3 -I tools/installs.py` prints totals by board, edition and week for beta and production.
- **Daily install email:** a separate Worker, `workers/install-email/` (Pages Functions can't run on a schedule), wakes
  at 15:00 UTC, reads both installs tables, and emails yesterday's counts to one address. It skips days production
  had none. The address is the Worker's `TO` secret (`tools/deploy_email.py to ADDRESS`), never in the repo, and
  Cloudflare Email Service delivers only to addresses confirmed in the account. `make deploy-email` uploads
  origin/master's copy and logs it with channel `install-email`. The token needs Workers Scripts, Edit.
- **Cloudflare plan:** Workers Free (Chad, 2026-11-05: Workers Paid, $5 a month, isn't needed). Everything here fits
  the free limits, and the install email goes only to a confirmed address, which is free. Ask Chad before building on
  a feature that needs Paid, such as email to any address.
- **Credentials:** `~/.config/cloudflare/outrigger-pages.env` holds `CLOUDFLARE_API_TOKEN` (Account, Cloudflare Pages,
  Edit; Account, D1, Edit) and `CLOUDFLARE_ACCOUNT_ID`. wrangler is pinned in `tools/deploy_site.py` and runs through `npx`.
- **Domains:** `outrigger.music` is the project's production custom domain. `beta.outrigger.music` is a custom domain
  whose proxied CNAME points at the beta branch alias, `beta.outrigger.pages.dev`
  ([Cloudflare's branch-domain guide](https://developers.cloudflare.com/pages/how-to/custom-branch-aliases/)). The
  zone's MX, SPF and DKIM records belong to Email Routing (feedback@outrigger.music).
- **One-way choice:** a Direct Upload project can't be switched to Cloudflare building on every push. That would mean a
  new project.
