# Public release audit (2026-10-31)

Chad decided to make `chadthornton/usb-midi-host` (now `chadthornton/outrigger`) public at beta start, after this check. It covers the working
tree and the full history: 243 commits on every local and remote branch, plus GitHub's `refs/pull/1..12/head`.

**Verdict:** the history holds no M-VAVE firmware, no JieLi loader and no secrets. It holds two low-sensitivity
personal items, Chad's FM-1 unit ID and his commit email. This branch fixes the working tree. A history rewrite
was optional; Chad chose to leave the history as is (2026-10-31). The project is GPL-3.0 (Chad, 2026-10-31), partly because one ported file descends from
GPL-3.0 code.

## How it was checked

- **Paths:** every path ever committed (`git log --all --name-only`). None ends in `.fwsc`, `.bin`, `.elf`,
  `.fm1bak` or `.fm1save`, and none is named like a dump or backup.
- **Blobs:** every blob in history sorted by size. The largest is EspUsbHost's source (600 KB). No blob is binary
  firmware. Every historical version of `tests/golden/*` holds command headers and SHA-256 hashes, never raw sector
  bytes (longest line 291 characters, a hash line in `packages.txt`).
- **Secrets:** gitleaks 8 over `--all` history: 207 commits, no leaks. gitleaks skips merge diffs. The 35 merge
  commits account for the gap to 243, and a search of their combined diffs for tokens and passwords found nothing.
  gitleaks over the working directory found only ESP-IDF's public example key
  `CONFIG_TEST_EVENT_TRIGGER_ENABLE_KEY="00112233…"` in the gitignored `build/` sdkconfigs. That is not a secret.
- **Personal data:** history search for the unit ID, `/Users/`, email addresses, SSIDs and passwords, Cloudflare
  credentials, IP addresses and serial numbers. The three committed `.DS_Store` files (removed in `bdad741`) were
  also read: they list only folder names (`build`, `firmware`, `lib`, `tests`).

## Findings

### 1. M-VAVE firmware: none committed (task repo-mvave-files)

| What | Where | Fix |
|---|---|---|
| Stock `FM-1_015.fwsc` (V15), Skiff, SLOOP and Felucca packages | Downloaded at test time by `tools/fetch_fixtures.py` into the gitignored `tests/fixtures/`, each pinned by SHA-256. Never committed on any branch. | None needed: already a download-at-build-time step. |
| Golden vectors `tests/golden/*.txt` | Command headers plus 8-byte hashes of each payload, made by `make golden` | None: they hold no firmware bytes. |
| Flash dumps and FM-1 backups | None in the repo. The 2026-10-25 dump lives in the separate recovery project. | None. |

### 2. Secrets: none (task repo-secrets)

| What | Where | Fix |
|---|---|---|
| Cloudflare Pages token | `~/.config/cloudflare/outrigger-pages.env`, outside the repo. The repo names only the variables; `tests/test_deploy_site.py` (PR #12) uses the dummy values `tok` and `acct`. | None. |
| Wi-Fi credentials | Entered on the device and saved in its NVS; the hotspot password is generated. No SSID or password in any file or commit. | None. |
| `CONFIG_TEST_EVENT_TRIGGER_ENABLE_KEY` | Gitignored `build/` sdkconfigs. ESP-IDF's public default. | None. |

### 3. Personal data (task repo-personal)

| What | Where | Fix |
|---|---|---|
| FM-1 unit ID `69bb…bd10` (Chad's unit, from its flash) | `loader_app.cpp` (`kDumpUnit`, shown as "(dump's)"), `loader_messages.h`, two tests, `docs/firmware-loader-plan.md` | **Fixed here:** replaced with `0123456789abcdef` in tests and comments, and removed from the loader and the plan doc. It stays in old commits; see the history decision. |
| SHA-256 of Chad's boot head and BT+voices region (`kDumpHead`, `kDumpFactory`) | `loader_app.cpp`, checkpoint-1 display | Kept: a hash of a 76 KB region reveals nothing about it. |
| Commit email `chad@brightlycoloredfood.com` | All 243 commits | None needed: the same email is already on 3,738 public commits across 11 repos. For future commits, GitHub's noreply address (`136056+chadthornton@users.noreply.github.com`) is an option. |
| "Chad's unit", dates of his recovery, quotes in `docs/learnings/robustness.md` (PR #3) | Docs | Not personal data, but PR #3's two quotes need Chad's look before it merges (tracker task repo-pr3). |

The tier for the `loader_app.cpp` change is light: it changes one display string ("Unit …") and nothing on a
write path.

### 4. Licenses (task repo-licenses)

**Vendored, ported and linked code:**

| Component | Where | License | In the firmware? | Obligation |
|---|---|---|---|---|
| EspUsbHost 2.10.1 | `lib/EspUsbHost/` (vendored, one patch) | MIT, TANAKA Masayuki | yes | notice; LICENSE kept |
| esptool-js 0.7.0 | `site/vendor/` (replaced ESP Web Tools 10.4.0 when `esptool-installer` merged) | Apache-2.0 | no (site) | LICENSE kept |
| Commissioner font | `site/fonts/` (from the site redesign) | SIL OFL 1.1 | no (site) | notice: **added** |
| MvaveFM1Unbricker | ported: `softkey.*`, `package.*`, part of `crypto.*`, `mock_rom.*`, `tools/fm1lib.py`, `tools/catalog_dedupe.py` | MIT, Michael P Jones | yes | notice: **added** (NOTICE plus a header line in each file) |
| jl-uboot-tool | ported: `rom.*`, part of `crypto.*`; `make golden` imports it from a checkout | MIT, Andrey Grigoryev | yes | notice: **added** |
| LVGL 9.6.0 | Arduino library; `lv_conf.h` is its edited template | MIT | yes | notice: **added** |
| Arduino_GFX 1.6.9 | Arduino library | BSD, Adafruit | yes | notice **with the binaries**: **added** (`site/NOTICE.txt`) |
| Arduino ESP32 core 3.3.12 | toolchain | LGPL-2.1 | yes, statically linked | users must be able to relink: met once the repo is public, since anyone can rebuild with `make build-all` |
| ESP-IDF (inside the core) | toolchain | Apache-2.0, components permissive | yes | notice: **added** |
| FM1Kit (`ocean/FM1Kit`, MIT) | nothing ported; reviewed for the crash lead only | | no | none |
| m-bootloader | nothing ported; not referenced anywhere | | no | none |
| JieLi `wl82loader.bin` | never committed; fetched by `make fixtures`, refused by `build_site.py` | unclear (JieLi's) | **must not ship** | already guarded |

**Binaries already ship.** beta.outrigger.music already serves firmware, so the MIT and BSD notices must travel
with it, whatever happens to the repo. This branch adds `NOTICE`, has `make site` copy it to `site/NOTICE.txt`, and
links it from the installer page's footer. The next `make deploy-site` publishes it.

**One GPL thread.** `lib/fm1_loader/src/package.*` (`.fwsc`/UFW parsing), the CRC and the UFW header cipher are
ports of the Unbricker's `parse_fwsc`, `ufw_flash`, `ota_crc16` and `ota_jl_enc`. The Unbricker labels these as
ports of `firmware/src/ota.c`. That file is in Felucca, a fork of M-VAVE's `fm1_open`, and both are GPL-3.0.
The Unbricker is published as MIT, but code translated from GPL-3.0 may still carry GPL-3.0. The ported part is
small (package.cpp is 107 lines: a standard CRC-16, a key-0xFFFF XOR cipher and a container header layout), and it is mostly
a description of a file format, which copyright protects weakly. The risk is low, not zero.

**No other GPL code** is in the repo. The GPL-3.0 firmwares (Felucca, Skiff, SLOOP) are test downloads only, and the
board fetches packages from the Firmware Hub at the user's request.

**Decision (Chad, 2026-10-31): the project is GPL-3.0** (`LICENSE`). The reasons:
- It ends the `ota.c` question.
- It matches the FM-1 firmware community this is announced to: M-VAVE's `fm1_open`, Felucca, Skiff and SLOOP are
  all GPL-3.0.
- Every dependency above is compatible with it: MIT, BSD, Apache-2.0 and LGPL-2.1.
- Chad owns the copyright in his own code, so he can later relicense the core (USB host, routing, UI kit) under MIT
  for the toolkit pitch (task repo-core-helpers), as long as it leaves out the ported `ota.c` lineage.

The alternatives were MIT with the `ota.c` risk noted, or asking Quixotic7 first.

### 5. Tidy (task repo-tidy-ci-docs)

| What | Where | Fix |
|---|---|---|
| README described CI as off because Actions minutes cost money | `README.md` lines 26 and 112 | **Fixed:** says CI is free once the repo is public and still not part of the gate. |
| README said the site is "not published anywhere yet" | `README.md` Licensing section | **Fixed:** names beta.outrigger.music. |
| `deploy_site.py` repeats `build_site.py`'s `wl82loader.bin` scan | `tools/deploy_site.py` (from PR #12, now on `master`) and `tools/build_site.py` | **Fixed** (repo tidy, 2026-11-01): both call `build_site.py`'s `wl82_problem()` and `load_wl82_blob()`. |
| Re-enable CI after going public | `.github/workflows/ci.yml` | **Done** (repo tidy, 2026-11-01): CI runs on pushes to `master` and on PRs as an extra signal. The gate stays `make check`. |

## History rewrite: optional, not recommended

Only two items in old commits would need `git filter-repo`: the unit ID (present in 157 commits' trees) and the commit email.
- **Rewrite with** `git filter-repo --replace-text` (unit ID) and `--mailmap` (email). Every commit SHA changes,
  every branch and worktree must be re-pushed with force, and open PRs #2, #3, #10, #11 and #12 must be recreated.
  GitHub keeps `refs/pull/*/head` for old PRs and `filter-repo` can't reach them; only GitHub support can purge
  those.
- **Leave as is:** the unit ID identifies one FM-1 and unlocks nothing, and the email is already public.

Recommended: leave the history as is. Chad chose to leave it as is (2026-10-31).

## Branches not on `master`

Scanned, not edited:
- **Clean:** `beta-docs-loader` (PR #11), `wifi-fallback` (PR #10), `claude/cloudflare-pages-f4323f` (PR #12),
  `hub-poll-timeout` (PR #2), `esptool-installer`, `claude/docs-learnings-0bd4bd` (PR #3) and the spike branches.
  None has firmware, secrets or emails.
- **Unit ID:** older copies of `loader_app.cpp` and `docs/firmware-loader-plan.md` on branches that predate this fix
  still have it. Any branch that rebases onto `master` drops it.
