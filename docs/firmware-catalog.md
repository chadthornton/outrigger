# The firmware catalog

Where Outrigger's list of FM-1 firmwares comes from, how a firmware's family decides what a switch does with
its data, and what the daily ecosystem check watches.

## Where the list comes from

**Add firmware** on the homepage (`site/installer/add.js`) reads [mutant.cloud](https://mutant.cloud), a community
catalog run by konsumer ([konsumer/mutantcloud](https://github.com/konsumer/mutantcloud), MIT). fm1buddy reads it too.
Makers sign in with GitHub and keep their own listings up to date.

- **The list:** `catalog.json?device=fm1`, in the Firmware Hub's format, FM-1 packages only.
- **Families:** `firmware.json?device=fm1&kind=firmware`. Its `base` says what each firmware is built on
  (`felucca`, `sloop`, `dinghy`, `original`...). The page shows it as "Felucca family", with the license.
- **The files:** `mutant.cloud/dl/<entry>/<file>`. mutant.cloud fetches the maker's file and serves it with CORS,
  and it serves only listed files. GitHub's release links can't be fetched from a web page.
- **Checks:** the page checks every file against the list's SHA-256, then the board checks it again before storing
  it. Nothing is hosted on outrigger.music.

**Fallback.** If mutant.cloud fails, takes more than 5 seconds or sends an empty list, the page reads
[thefirmwarehub.com/catalog.json](https://thefirmwarehub.com/catalog.json) instead, as it did before, and says
which list it shows.

**Project ids stay the Hub's.** The board keeps each firmware's saves on a shelf named by its project id, so the
page keeps those ids stable whichever list it reads:

- A listing's `entry` is its project (`skiff`, not mutant.cloud's row id `skiff-fm-1-722`).
- Four listings mutant.cloud renamed map back to the Hub's ids: `b1`, `demake`, `fm1dock` and `fm1sid`
  (`HUB_ID` in `site/installer/add.js`).
- A firmware with several FM-1 files (a Bluetooth build, a one-core build) keeps its id for its main file. Each
  other file is a project of its own, so two different files never share a shelf.

The installer's own downloads don't use either catalog. A fresh install fetches only stock V15, from M-VAVE's own
repo (`m-vave/fm1_open`) at a pinned commit, and JieLi's `wl82loader.bin`, from `jl-uboot-tool` at a pinned commit,
both on GitHub and each pinned by SHA-256 in `tools/store_template.py`. Stock moved off the Hub on 2026-11-05: the
Hub keeps only each project's newest file, and M-VAVE's current download is V16.

## Families and switching

`tools/fm1_families.json` is the reviewed table. A listing's `base` is self-serve on mutant.cloud, so the table's
ids override it. Melodee, Hortator, Jangada and Salt, for example, are listed there as built on Felucca.

Lookups follow "built on" through firmwares that are themselves bases: Tribe → X0X → Felucca.

| Family (`base`) | Group | On the Add firmware list |
|---|---|---|
| stock, felucca | Known (tested on hardware) | Yes |
| fm1base, dinghy, original, foma | Clean slate | Yes |
| ids sloop and its `USR4` forks (clipper, spampy, winampfm, floyd, nmsfm, karma, sloopbanks, fm1quest), melodee, tessera (`riskier_ids`) | Riskier: clean slate, after a warning once per unit | Yes, under "Riskier" |
| built on sloop, melodee or a riskier id, unreviewed; ids jangada, gator, vector37, mycelium | Blocked | No, held with the reason |
| ids the survey cleared (`cleared_ids`): sloopdx, salt, hortator, mnm1, nauclerus, hawkchua | Clean slate, whatever their base | Yes |
| anything else, or none listed | Unknown | Yes |

**What "data kept per firmware" means.** Every firmware's data is saved on its own shelf before a switch and comes
back when you switch back to it (`docs/package-sources.md`).

**What "clean slate" means.** A clean slate also erases update staging (`0xE0000–0xE4FFF`), so a stale update record
never survives a switch, and the board asks before it erases.

Today every switch between two different firmwares is a clean slate unless that exact pair was tested on hardware
(`kTestedPairs` in `lib/fm1_loader/src/library.cpp`).

**The Felucca family keeps the clean slate too** (orchestrator, 2026-11-04): trusting it would only skip the
staging erase and the prompt, and a stale update record must not survive a switch.

**Who holds what.** The Add firmware page holds every blocked firmware off its list, and the board refuses the same
list (`lib/fm1_loader/src/families_gen.h`, generated with the page's `site/installer/families_gen.js` by
`tools/gen_families.py`).

**Blocked, and why:**

- **Firmwares built on SLOOP, Melodee or a riskier id that nobody has reviewed** may write `0xE9000` or outside the
  clean slate. Each comes off the list after a source read, into `riskier_ids` or `cleared_ids`.
- **Jangada, Gator and Vector37** write outside the clean slate; **Mycelium** has no source.

**Riskier, and why:** SLOOP 2.4+ and the forks with its fourth sample slot (`0xE7000–0xFAFFF`), Melodee and Tessera
may overwrite `0xE9000`, the sector whose hash is how Outrigger tells FM-1s apart, and nothing else outside the clean
slate. They can't brick the FM-1 (Chad, 2026-11-05), so they install after a warning; how the board copes when the
sector changes is in `docs/package-sources.md`. The page also holds a firmware the snapshot doesn't know when its
catalog base is one of these (`HELD_BASES`).
- **Cleared by id:** six of that family write only inside the clean slate (read at their release tags, 2026-11-04),
  so `cleared_ids` lets them through whatever their base says. Each is cleared for the release tag we read; the daily
  check flags a newer one.

## The daily ecosystem check

`.github/workflows/ecosystem-check.yml` runs `tools/ecosystem_check.py` once a day on GitHub, and on demand from the
Actions tab. It needs no Claude session and no Mac, and it uses only the default `GITHUB_TOKEN` (issues: write).

1. **Installer links.** It downloads every file and list the live installer depends on: stock V15 and
   `wl82loader.bin` from their pinned GitHub commits (checked against their pinned SHA-256), and both catalogs. If any is broken, the run fails,
   and GitHub emails a red mark.
2. **New FM-1 releases since the last run,** from mutant.cloud's
   [feed](https://mutant.cloud/feed.xml?device=fm1) and `firmware.json`, riskiest first:
   1. new or unknown family, or a package file that went missing from its maker's link;
   2. blocked;
   3. clean-slate families;
   4. known families.
3. **One issue, "Ecosystem check"** (label `ecosystem-check`). Each run rewrites its body:
   - broken links come first, then the groups above;
   - what the last run saw rides in a hidden comment in the body.

   It adds a comment that @-mentions the owner, which is what notifies you, only for:
   - a broken link;
   - a newly missing file;
   - a family the table doesn't know;
   - a listing renamed on mutant.cloud;
   - a new release of a firmware in `cleared_ids` (read its flash map again, or move it back to `blocked_ids`).

What to do with what it reports:

- **A broken installer link:** fresh installs fail. Bump the pin in `tools/store_template.py` (and
  `tools/fetch_fixtures.py`), then redeploy the site.
- **A new family:** decide its group, then add it to `tools/fm1_families.json` (and to the page's held lists in
  `site/installer/add.js` if it's blocked).
- **A rename:** add it to `HUB_ID` in `site/installer/add.js`.
- **A missing file:** the Add firmware list shows what mutant.cloud lists, and mutant.cloud marks the file missing.
  Nothing to do unless it's in the starter set.

GitHub turns off scheduled workflows on a public repo after 60 days without a commit. It emails a warning first:
re-enable the workflow from the Actions tab.
