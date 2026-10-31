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
holds a save restores the latest one. So Felucca → Melodee → Felucca brings the Felucca songs back, and no
switch loses data, even when it starts fresh.

| Installing | Data |
|---|---|
| A newer or older version of the **same catalog project** (matched by `ident`) the FM-1 runs now | Keep, unless the versions cross a known data-format break (a table we keep, starting with Skiff 1.x → 2.x: fresh start) |
| A **different** catalog project with a save on its shelf | Restore that project's latest save |
| A **different** catalog project with an empty shelf | **Fresh start** (the old project's data waits on its own shelf) |
| A sideloaded file, or firmware no catalog entry matches | Fresh start only, behind an extra "not from the catalog" warning; its shelf is its file hash |
| Back to **stock** | Restore stock's shelf, or fresh start if it's empty |

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
- **Which flash a shelf holds is unchanged:** Felucca's save ranges for every community firmware (verified
  from Felucca's source and checkpoint 3), stock still unmapped (kept in place, never saved or erased).
  - **Known gap:** a firmware that keeps data outside Felucca's ranges leaves it in place across a fresh
    start, and its shelf doesn't hold it. The whole-flash backup before every install still does.
- **The format-break table** starts with one row: Skiff below 2.0 to 2.0 or later is a fresh start (or that
  project's shelf, if it holds a 2.x save).

## Checks that keep this honest

- **`make check-latest`,** run at the start of each loader session and before installing a new release: each
  pinned firmware's newest release through a mock install. `LATEST_ALL=1` covers the whole catalog.
- **Mac tests for the trust table:** every row, including `bubba`'s `FM-1_003` and a sideloaded file.
- **A hardware checkpoint** before the first upload install, before the first Wi-Fi download install, and
  before any project pair joins the allowlist.

## Order of work

1. ~~**Identity parsing.**~~ Done: `FM-1_` and 3 to 15 digits, all the package marks hold.
2. **The trust table and project shelves** in the installer's data decision, with tests. Packages still come
   from `make store-add` until step 3; this step only makes installs safer.
3. **Upload on the web page,** with the catalog hash check and the sideloaded mark. Then a board checkpoint.
4. **Catalog and Wi-Fi download,** with the last-known-good cache, "Check for updates" and the prerelease
   setting.
5. **Then the session 3b robustness items:** USB read recovery, then MFLR.

## Decisions (Chad, 2026-10-31)

1. **Firmware never tested on hardware:** offered, with a fresh start (its old data waits on its shelf).
2. **First way in:** upload on the web page; Wi-Fi download after.
3. **Prereleases:** behind a setting, off by default; a beta shows only when it's newer than the stable release.
4. **Data across projects:** none at launch. Same project only keeps data; every other switch restores that
   project's shelf or starts fresh.
