# Package sources: getting new FM-1 firmware onto the board

Agreed with Chad 2026-10-31 (decisions at the end). Nothing here is built yet. This is guarded work: it decides what the
board may write to an FM-1 and what happens to the user's songs and settings.

## Goal

A user gets new FM-1 firmware as authors release it, without a Mac. The board downloads it, or the user
uploads a file. Each package is checked before it's stored and again before it's installed, and an
unfamiliar firmware can never be installed over another firmware's saved data.

## What exists today

- **Packages enter the store only from a Mac:** `make store-add` over USB in Computer mode.
- **The loader handles new releases already:** Skiff 2.0.3, Felucca 1.7.0 and SLOOP 2.8.4 parse and
  install over stock on the mock FM-1 with no code change (`make check-latest`).
- **The parser rejected 49 of the catalog's 130 firmwares.** It wanted an identity of exactly `FM-1_NNN`, but
  catalog firmwares use 3 to 10 digits (`x0x` is `FM-1_9010100`). Step 1 below fixed it: 129 pass. Vesper
  stays refused: its `flash.bin` doesn't reach the end of the app area.
- **The family rule is too coarse to be safe.** `family_of()` calls any identity starting `FM-1_0` stock and
  everything else Felucca. Two examples:
  - `bubba` (`FM-1_003`) counts as stock, so installing it would keep stock's data.
  - SLOOP, which is Felucca 0.9-based, counts as Felucca, so Felucca's restore rules apply to it.
  - In both cases the loader keeps or restores data for firmware it has never been shown to understand.

## The catalog

The Firmware Hub's `catalog.json` is the one index. For each firmware it already carries:
- `id` and `version`;
- `pkg` (the hub's file) and `sha256`;
- `product` (the identity the package reports) and `ident` (a pattern for every identity the project has used);
- `repo`, `tag` and `release` (the author's GitHub release);
- `lic`, and a description that sometimes notes data-format changes (Skiff: "2.0 saves in new formats").

The board keeps a last-known-good copy of the catalog in its store and shows its age. It fetches a new one
only when the user asks ("Check for updates"). It never calls the GitHub API: each install would spend rate
limit, and the catalog already holds what the API would return. (Schwung, Ableton Move's update tool, works
the same way; its trust model is the part not to copy, since it checks no hashes.)

## Two ways in

1. **Download (Wi-Fi):** the board fetches the package from the hub's `pkg` URL, falling back to the
   author's GitHub release asset (`repo`, `tag`). About 600 KB goes into PSRAM, then into the store.
2. **Upload (web page):** the user picks a `.fwsc` file on the board's page.

Either way, before anything is stored:
- the SHA-256 is checked against the catalog;
- the package checks run: identity, layout, and the app isn't blank.

A package whose hash matches no catalog entry is stored as **sideloaded** and stays marked as such.

## The trust rule (the decision this design exists for)

What an install may do with the FM-1's songs and settings depends on what the loader knows. It doesn't
depend on what a package claims.

**Every project gets its own shelf.** Before any install, the board backs up the flash and autosaves the
running firmware's data into the store, filed under its catalog project. Installing a project whose shelf
holds a save restores the latest one. So Felucca → SLOOP → Felucca brings the Felucca songs back, and no
switch loses data, even when it starts fresh.

**Only a tested pair switches the family map; every other switch is a clean slate** (Chad, 2026-11-01, from the
firmware map scope). A pair is tested when a hardware run proved it, by project and exact identity, either way
(`kTestedPairs`, `library.cpp`). Every other switch, after the backup and autosave, erases `0x94000–0xE8FFF` and
`0xFC000–0xFEFFF` (`Right::CleanSlate`), then writes the target's save into it if its shelf has one. It never
touches the boot head, the app area, `0x93000` (stock's factory data), `0xE9000` (the unit ID),
`0xEA000–0xFBFFF` (stock's resources) or key_mac. Everything it erases is in the outgoing autosave except update
staging `0xE0000–0xE4FFF`, which is never saved: a stale update record must not survive a switch. A clean slate
asks for the user's OK, like a fresh start.

| Pair | Status |
|---|---|
| Felucca 1.6.1 (`FM-1_916`) ↔ Skiff 1.4 (`FM-1_714`) | Tested (checkpoint 3, 2026-10-30) |
| Felucca 1.8.0 (`FM-1_918`) ↔ Skiff 2.1.1 (`FM-1_721`) | Owed: bench test 2. Clean slate until it passes |
| Stock V15 (`FM-1_015`) ↔ Felucca 1.8.0 | Owed: bench test 2. Clean slate until it passes |

**Backup-only, by boot head** (Chad, 2026-11-05). The board installs only on stock V15's boot head, CRC-32
`3A04AD23` over `0x0000–0x3FFF`, read from the unit's latest backup. That's the head on FM-A and FM-B, and the
one in the V15 package. Any other head can be backed up, but every install is refused before the key:
- M-VAVE's V16 (`08316064`, from r/MVaveFM1, not seen on a unit yet): "This FM-1 has M-VAVE's V16 start-up code."
- anything else: "This FM-1 has unknown start-up code."

Both add "Outrigger can back it up but not switch it yet." The reasons: V15-based firmwares and stock V15 itself
are untested on another head, and V16 keeps its Bluetooth address at `0xC0000`, inside the clean slate.

**Blocked, by family** (`families_gen.h`, generated by `tools/gen_families.py` from the reviewed
`tools/fm1_families.json` and a committed snapshot of what mutant.cloud and the Firmware Hub say each firmware is
built on, `tools/fm1_lineage.json`). The board never reads a catalog, so a listing's edit changes nothing until
someone refreshes the snapshot, reviews the header's diff and ships a build. Our own survey's ids win where a
catalog disagrees. The board refuses two kinds of firmware:

- **Melodee, Tessera and firmwares built on Melodee** ("Not yet: it writes the FM-1's Bluetooth sector"). They can
  write `0xE9000`, the sector whose hash is the unit ID. After one of their saves, the board would see a different
  FM-1 and orphan its backups.
- **SLOOP, firmwares built on it, and the Felucca 0.9 survey** ("Not yet: it's built on an older Felucca"). Going
  from one of them to Felucca or Skiff could leave data that crashes them; the clean slate now erases that data.
  What keeps SLOOP 2.4+ blocked is its fourth sample slot, `USR4` at `0xE7000–0xFAFFF`: clearing or filling it
  erases `0xE9000`, the Melodee problem above. The same slot is in Clipper, SPAMPY, winampFM, Floyd, No Man's Sky
  FM, Karma, SLOOP Banks 2.5 and FM-1 Quest.
- **Cleared by our survey** (`cleared_ids` in `tools/fm1_families.json`): DX SLOOP, Salt, Hortator, MNM-1,
  Nauclerus and Hawk-Chua. Read at their catalog release tags (2026-11-04), their flash drivers erase and write only
  inside the clean slate, so they switch like any untested firmware even where a catalog says they're built on
  SLOOP. The daily ecosystem check flags a release newer than the one read.

Switching away from a blocked firmware works. A firmware's other files (a Bluetooth build, a one-core build) are
blocked with it. A sideloaded copy (no catalog project) isn't caught: an accepted gap.

**Skiff 2.x and stock's resources.** Skiff 2.1.1's pattern bank rotates through `0xEA000–0xFBFFF`, where stock
V15 keeps resources the board can't put back. Switching to stock on a unit that runs or ran Skiff 2.x warns
"Returning to stock may lose stock resources" until bench test 1 says what V15 does without them.

| Installing | Data |
|---|---|
| A newer or older version of the **same catalog project** (matched by `ident`) the FM-1 runs now | Keep, unless the versions cross a known data-format break (a table we keep, starting with Skiff 1.x ↔ 2.x: that project's shelf for the other era, or a clean slate) |
| A **different** catalog project with a save on its shelf | Restore that project's latest save (on a clean slate unless the pair is tested) |
| A **different** catalog project with an empty shelf | **Fresh start** (the old project's data waits on its own shelf), on a clean slate unless the pair is tested |
| A sideloaded file, or firmware no catalog entry matches | Clean slate; its shelf is its file hash |
| Back to **stock** | Clean slate (stock saves are disabled, so nothing comes back): no pair to stock is tested yet |
| **Melodee**, **Tessera**, **SLOOP** or their families, or the Felucca 0.9 survey, except the ids our survey cleared | Refused (above) |

- **No data is kept across projects at launch, not even Felucca ↔ Skiff** (Chad, 2026-10-31). A pair can
  later join an allowlist that keeps data across it, after a board test shows the data surviving both ways
  (about 10 minutes per pair).
- **A shelf belongs to a project, not an identity.** Felucca 1.6.1 reports `FM-1_916` and 1.7.0 `FM-1_917`:
  same project, same shelf. Today's saves are keyed by identity, so this changes the save key.
- **Identity becomes a lookup, not a digit test.** The installed project comes from matching the FM-1's
  identity against the catalog's `ident` patterns (stored with the backup). `family_of()`'s digit test goes.
- **No downgrades offered as updates.** "Update available" means a strictly newer catalog version of the
  same project. The installed version recorded is the catalog's, written only after the install verifies.
  The verify is today's read-back plus finish_record.

## Step 2, how it's built

- **A package knows its project.** `store-add` (and later upload and download) looks the file's SHA-256 up
  in the catalog and records the catalog `id` in the package manifest (`project`). A file the catalog doesn't
  list is sideloaded: its project is `sha:` and the first 16 hex digits of its app hash.
- **The running firmware's project comes from the store, not from patterns.** `identify_app` already
  matches the FM-1's app area against the stored packages sector by sector; the matched package's project is
  the running project. An app no stored package matches is unknown: every install over it starts fresh. No
  regex runs on the ESP32.
- **Saves carry the project.** Saves recorded before this (s4 to s51 on the board) have none. They get it
  when read, from the stored package with the same app hash, so nothing is rewritten.
- **Stock is the catalog's `stock` project**, not "an identity starting `FM-1_0`". `bubba` (`FM-1_003`) is
  a community firmware, with Felucca's ranges like the others.
- **Which flash a shelf holds:** one map for every community firmware: Felucca's save ranges (verified from
  Felucca's source and checkpoint 3) plus what other firmwares on the shelf write, which is the clean slate
  minus staging: `0x94000–0xDFFFF`, `0xE5000–0xE8FFF`, `0xFC000–0xFEFFF`. Until 2026-11-01 it missed
  `0x9F000` (Felucca 1.x's FM6 user presets, copy A, and SLOOP's autosave), `0xE8000` (Skiff's second bank
  sector) and `0x94000–0x96FFF` (jangada, XMOD, fm1-dock). A save made before then restores those sectors
  blank. Stock is still unmapped: never saved.
  - **Known gap:** a firmware that writes `0xE9000` or `0xEA000–0xFBFFF` (Skiff 2.x, Melodee, Tessera, and
    the sample and bank writers in `fw-map-bench.md`) leaves that data in place, and no shelf holds it. The
    whole-flash backup before every install still does.
- **The format-break table** starts with one row: Skiff below 2.0 and 2.0 or later are different eras. Skiff
  2.0 can read and convert 1.x data (its release notes), but the board hasn't tested that, so crossing the
  break in either direction restores that era's shelf or starts on a clean slate. 1.x songs wait on their
  shelf.

## Checks that keep this honest

- **`make check-latest`,** run at the start of each loader session and before installing a new release: each
  pinned firmware's newest release through a mock install. `LATEST_ALL=1` covers the whole catalog.
- **Mac tests for the trust table:** every row, including `bubba`'s `FM-1_003` and a sideloaded file.
- **A hardware checkpoint** before the first upload install, before the first Wi-Fi download install, and
  before any project pair joins the allowlist.

## Order of work

1. ~~**Identity parsing.**~~ Done: `FM-1_` and 3 to 15 digits, all the package marks hold.
2. ~~**The trust table and project shelves.**~~ Done, and its board checkpoint passed 2026-10-31 (build
   `1737c9b`). `store-add` recorded the projects of the board's five packages: Felucca 1.6.1, Skiff 1.4 and
   SLOOP 2.8.1 were proven byte for byte against their GitHub releases. Then, on the FM-1:
   - Back up: b89, nothing written. The FM-1 ran SLOOP 2.8.1.
   - SLOOP → Felucca 1.7.0: restored Felucca's shelf (a save made under 1.6.1) and filled SLOOP's. Chad then
     made a song.
   - Felucca 1.7.0 → 1.6.1: refused first ("the FM-1 changed since b91", nothing written: the new song); after
     a backup it switched, and the song was kept.
   - → SLOOP 2.8.1: SLOOP's shelf came back; the song went to Felucca's shelf.
   - → Felucca 1.7.0: Felucca's shelf came back, with the song.
3. **Upload on the web page,** with the catalog hash check and the sideloaded mark. Then a board checkpoint.
4. **Catalog and Wi-Fi download,** with the last-known-good cache, "Check for updates" and the prerelease
   setting.
5. **Store space: packages are a cache, user data isn't** (below). With step 4, since downloads are what
   fill the store.
6. **Then the session 3b robustness items:** USB read recovery, then MFLR.

## Store space

Measured 2026-10-31 on the board: 5 packages, 3 backups and 7 saves use 5.2 of the store's 12.7 MB. The
store keeps each 4 KB sector once, so versions of one project share most of their sectors, a backup shares
its app with the installed package, and a save costs only the sectors its data changed (tens of KB). A new
firmware costs about 0.6 to 1 MB: room for roughly 8 to 12 more today. RAM isn't the limit: a package is
fetched into PSRAM (8 MB) before it's stored.

- **Packages are a cache.** Every catalog version stays on its author's GitHub release, so an unpinned
  package can go when space runs low, least recently installed first, and download again later. Pinned
  packages (the star) and the one each unit runs never go. A sideloaded package can't download again, so
  it's never evicted either; the page offers to remove it.
- **Backups and saves are the user's, and never go by themselves.** The first backup of each unit (its
  baseline) and the newest save on each project's shelf always stay. Older ones are listed with their size,
  and the user trims them after exporting to a laptop (`.fm1bak`/`.fm1save`, the Web UI step).
- **Install without keeping it.** A download can install and then leave the store, for a firmware the user
  is only trying. Its shelf of saves stays.
- **Before a download or upload,** the board checks the space it needs, evicts cached packages if that's
  enough, and otherwise stops and says how much to free. It never fills the store mid-write.
- **Later, if needed:** store packing (per-sector files cost about 1.8x their data today) and a TF card
  tier (`docs/architecture.md`).

## Decisions (Chad, 2026-10-31)

1. **Firmware never tested on hardware:** offered, with a fresh start (its old data waits on its shelf). From
   2026-11-01 that fresh start is a clean slate, and Melodee and Tessera are refused (above).
2. **First way in:** upload on the web page; Wi-Fi download after.
3. **Prereleases:** behind a setting, off by default; a beta shows only when it's newer than the stable release.
4. **Data across projects:** none at launch. Same project only keeps data; every other switch restores that
   project's shelf or starts fresh, on a clean slate unless the pair is tested.
