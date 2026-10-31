# Package sources: getting new FM-1 firmware onto the board

Draft for Chad's review, 2026-10-30. Nothing here is built yet. This is guarded work: it decides what the
board may write to an FM-1 and what happens to the user's songs and settings.

## Goal

A user gets new FM-1 firmware as authors release it, without a Mac. The board downloads it, or the user
uploads a file. Each package is checked before it's stored and again before it's installed, and an
unfamiliar firmware can never be installed over another firmware's saved data.

## What exists today

- **Packages enter the store only from a Mac:** `make store-add` over USB in Computer mode.
- **The loader handles new releases already:** Skiff 2.0.3, Felucca 1.7.0 and SLOOP 2.8.4 parse and
  install over stock on the mock FM-1 with no code change (`make check-latest`).
- **The parser rejects 49 of the catalog's 130 firmwares.** It wants an identity of exactly `FM-1_NNN`, but
  many use 4 to 7 digits (`x0x` is `FM-1_9010100`). One more fails on a short `flash.bin`. 80 pass.
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

| Installing | Data |
|---|---|
| A newer or older version of the **same catalog project** (matched by `ident`) the FM-1 runs now | Keep, unless the versions cross a known data-format break (a table we keep, starting with Skiff 1.x → 2.x) |
| A project on our **allowlist** of proven pairs (to start: Felucca ↔ Skiff, which checkpoint 3 ran both ways) | Keep or restore, as today |
| Any other catalog firmware | **Fresh start** only: the backup and an autosave go to the store first, so the old data can come back by reinstalling the old firmware |
| A sideloaded file, or firmware no catalog entry matches | Fresh start only, behind an extra "not from the catalog" warning |
| Back to **stock** | Fresh start, unless the FM-1 ran stock before (then restore its stock save) |

- **A pair joins the allowlist only after a hardware checkpoint** shows data surviving in both directions.
- **Identity becomes a lookup, not a digit test.** The installed project comes from matching the FM-1's
  identity against the catalog's `ident` patterns (stored with the backup). `family_of()`'s digit test goes.
- **No downgrades offered as updates.** "Update available" means a strictly newer catalog version of the
  same project. The installed version recorded is the catalog's, written only after the install verifies.
  The verify is today's read-back plus finish_record.

## Checks that keep this honest

- **`make check-latest`,** run at the start of each loader session and before installing a new release: each
  pinned firmware's newest release through a mock install. `LATEST_ALL=1` covers the whole catalog.
- **Mac tests for the trust table:** every row, including `bubba`'s `FM-1_003` and a sideloaded file.
- **A hardware checkpoint** before a project pair joins the allowlist, and before the first Wi-Fi download
  install.

## Order of work

1. **Identity parsing:** accept the catalog's 3-8 digit identities. Mac tests; the one short `flash.bin`
   stays rejected.
2. **The trust table** in the installer's data decision, with tests. Until steps 3 and 4 exist, packages
   still come from `make store-add`; this step only makes installs safer.
3. **Upload on the web page,** with the catalog hash check and the sideloaded mark.
4. **Catalog and Wi-Fi download,** with the last-known-good cache and "Check for updates".
5. **Then the session 3b robustness items:** USB read recovery, then MFLR.

## Decisions for Chad

1. **Unknown firmware:** offer it with fresh start (as above), or hide it until it's on the allowlist?
2. **First way in:** upload on the web page first (smaller, no TLS on the board), or download first?
3. **Prereleases:** the catalog lists betas (`bubba-0.10.0-beta`, `melodee-1.6.0-rc.7`). Show them, hide
   them, or behind a setting?
4. **Starting allowlist:** Felucca ↔ Skiff only, or also SLOOP ↔ Felucca once its data map is known (it's
   the known brick pattern today)?
