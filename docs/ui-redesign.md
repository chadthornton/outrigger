# UI redesign: top-level nav, errors, what's running (draft for Chad's sign-off)

**Status:** proposal, 2026-10-30. Mockups only, nothing built into `host_ui` yet. Build it after `loader-session3`
merges (ui-polish rewrote `host_ui`, and that merge carries the current Firmware page).

- **Review page:** https://claude.ai/artifact/AemH6yE3Nzyeu6GYobHaeg (every screen at S, M and L).
- **Mockups:** `make ui-mocks` renders them with LVGL from `tests/ui_mocks/ui_mocks.cpp` into `build/ui-mocks/`,
  with the same label checks as `make ui-shots`.
  - **Results:** 45 screens (15 at each size), 0 layout problems. Every page fits at S and M; at L, 12 page bodies
    scroll, but their holds and the tab bar stay in view.
  - **Sample data only.** The mock code is a layout reference, not code to merge into `host_ui`.

**Chad's goals:** "a top level nav and better error messaging and better clarity around what's currently running
and what the backup process is."

## What's wrong today

Baseline: `loader-session3` at `154d147`, which split the Firmware page into a list and a detail screen holding the
status lines, the preview and the 2-second hold. That fixed the overflow and matches this design's list → detail
direction. What's left:
- **Navigation is hidden.** A tap anywhere on the chain opens Settings, and Firmware is a button inside Settings.
- **Nothing says what the board is doing.** No mode is shown outside the Firmware detail's title.
- **Errors mix three things.** The stopped detail shows nine lines at one weight: cause, USB codes, sector, the
  danger warning and advice. The board knows whether the FM-1 is safe and what fixes it, but doesn't say so first.
- **"Running" has no source.** The list marks a package "running", but that mark comes from the latest backup, not
  from the FM-1. Skiff even calls itself Felucca on USB.
- **Diagnostics sit in the main flow.** Store, blob, unit hashes and free RAM appear on every detail screen.

## Screen map

```
Play (chain) ── FM-1 home ──┬── Back up (what it is + hold) ──┐
                            ├── Firmware list ── Install X (plan + hold) ──┤
                            └── Last job (result / error + fix hold) <──── Job screen (LOAD boot)
Setup ── Computer mode (hold)                                     └── Install stopped (retry hold, LOAD boot)
```

- **Three top-level sections** in a tab bar at the bottom of every top-level page:
  - **Play:** the signal chain.
  - **FM-1:** what it runs, backups, firmware.
  - **Setup:** board version, Web address, text size, Computer mode.
- **Sub-pages** (Firmware list, Back up, Install X, Last job, Computer mode) hide the tab bar and show a title row
  with a back arrow; a tap on that row goes back.
- **Job screens** (backing up, installing, install stopped) belong to a Load boot. They take the whole screen, with
  no tabs and no back, because routing isn't running and nothing else can be done.
- **After a job** the board restarts into Play and opens Last job (decision 3), whose OK goes to FM-1 home.
- **Tapping the chain does nothing.** The tab bar replaces that hidden gesture.
- **Swiping left and right** between the three sections is an optional shortcut (decision 1).

## The status strip

Every screen starts with one line, at 12 px (14 px at L), which doesn't grow with text size:
- **Left: a mode pill.**
  - **PLAY** (green): routing.
  - **LOAD** (yellow): a job's boot, and the right side reads "MIDI off".
  - **SAFE** (red): after a crash, when the USB host isn't started.
  - Computer mode keeps its own screen.
- **Right: what the FM-1 runs**, e.g. "FM-1: Skiff 1.4". It comes from `identify_app()` over the unit's latest backup,
  as `refresh_items()` does now.
  - **Unknown:** "FM-1: back it up".
  - **Never the USB name:** Skiff's USB name says Felucca.
- **The Power node** leaves the chain. Its line was constant ("USB-C 5V"), and a power problem still shows in the
  chain's problem card.
- **USB 8/8** moves into the Hub line.

## FM-1 section

- **An unfinished install overrides everything here.** When `resume_pending()` is true (the board booted with the
  FM-1 still in ROM boot and a pending record), FM-1 home becomes the "Install unfinished" card. Its retry hold is
  the only action; Back up and Change firmware are gone. This is checkpoint 3's mix-up: the split pages must not
  leave a backup holdable on a half-written FM-1.
  - **The strip** reads "FM-1: install unfinished" in red, since the latest backup no longer describes the FM-1.
  - **Play** shows the FM-1 node red, with "Install unfinished: see FM-1".

- **FM-1 home.**
  - **A card:** "Runs Skiff 1.4", then in small grey "Read from backup b50. Its USB name says Felucca."
  - **Two rows:** "Back up" (sub-line: "Latest b50 matches it") and "Change firmware" (sub-line: "5 in the library").
- **Firmware list.**
  - **One line per package:** name, a short note on the right, and a favorite star (decision 4).
  - **Notes:** "running" (green, with the row tinted), "save" (a save state exists, so installing brings its songs
    back), "not yet" (SLOOP, until its data map is known).
  - **Footer:** the store's free space, small.
  - **A tap on a row** opens Install X.
- **Back up.** Three lines, then the hold "Hold: back up this FM-1":
  - "Reads all 1 MB of the FM-1 twice and keeps the copy on this board."
  - "Writes nothing to the FM-1. Every install needs one first: it's the undo."
  - "MIDI stops until it's done."
- **Install X.** The preview from `preview_install()` in words, then the hold "Hold: install Felucca 1.6.1":
  - What it replaces, and how many sectors it writes.
  - What happens to songs: the decision's keep, restore or fresh start, with the layout warning.
  - "MIDI stops until it's done."

## Holds

- **What needs one:** anything that touches the FM-1 or stops MIDI is a 2-second hold, including backup and (decision
  2) Computer mode.
- **Where it lives:** pinned to the bottom of its own page, outside the scrolling body, so it never scrolls away.
- **The label names exactly what it runs:** "Hold: retry Felucca 1.6.1", never a shared "back up" label. The label
  comes from the same state the hold acts on (`previewed_id`, `previewed_retry`), as now.

## Job screens (Load boot)

- **Title:** "Backing up" or "Installing Felucca 1.6.1".
- **Steps:** one line each, with a check for done, an arrow for the current step, and a dash for steps to come. Then
  a progress bar.
  - **Backup:** Board in Load mode, FM-1 in ROM boot, Reading (pass and %), Compare and store, Restart the FM-1.
  - **Install:** Checked against b50, Saved Skiff's songs, Writing N of M, Restart the FM-1.
  - The steps group `Step` values: SendKey/WaitRom; Identify/UploadLoader/ReadIds; Backup or Plan/ResumeRead;
    WriteApp/WriteData/Check; Reset/WaitMidi.
- **Danger card:** a red "Don't unplug or switch off the FM-1" shows while `Installer::danger()` is true, and only
  then.
- **Backup footer:** "Writes nothing to the FM-1."

## Errors

**The pattern.** Every job error has the same parts, in this order:
1. **Headline:** what happened, orange (safe) or red (danger).
2. **Safety card,** in one of three states:
   - **Nothing written** (green check): the job erased and wrote nothing.
   - **Written and verified** (green check, "Installed and checked."): it wrote, every sector verified, and the
     error came after (`MidiNotBack`).
   - **Danger** (red warning, "Don't unplug the FM-1 or switch it off."): `danger()` is true.
   - The state comes from whether the job wrote anything (sectors written, or a write step reached), not from
     `!danger()` alone. `MidiNotBack` has `danger()` false after 149 sectors were written.
3. **Why:** one plain sentence.
4. **To do:** one sentence, then a hold that runs that fix and says which one.
5. **Detail:** small and grey, for us: `failure_detail()`, `scsi.last_failure`, `host.why`.

**Chain problems** keep the existing card on Play, with `status.h`'s what-and-do lines. Job errors live under FM-1.

**Mapping from today's signals** (headline / why / to do / fix hold):

| Signal | Safe? | Why | To do | Hold |
|---|---|---|---|---|
| `danger()` after `DeviceLost`, `WriteVerifyFailed` | no | "It's in ROM boot with half of X written." | "Hold to retry: it carries on where it stopped." Detail adds "If the retry fails: Transporter + bN." | retry X |
| `KeyNotSent`, `host.why` = "FM-1 already in ROM boot", **no pending record** | yes | "The FM-1 was already in ROM boot." | "Switch the FM-1 off and on, then try again." | back up / install X |
| The same, **with a pending record** for this unit | no | Route to "Install unfinished": never advise a power cycle. | | retry X |
| `KeyNotSent`, "no FM-1 on the hub" | yes | "No FM-1 on the hub." | "Plug it into hub port 2." | none (OK) |
| `NoRomDevice` (after the 3 tries) | yes | "The FM-1 took the key but didn't show up in ROM boot." | "Switch it off and on, then try again." | the same job |
| `BackupStale` | yes | "The FM-1 changed since backup bN: a saved song counts." | "Back up again, then install." | back up |
| `NoBackupForInstall` | yes | "This FM-1 has no backup yet." | "Back up first." | back up |
| `BackupMismatch` | yes | "The two reads differed." | "Try again. If it repeats, check the cable and hub." | back up |
| `LoaderTooSlow` | yes | "The FM-1 was too slow to read." | "Try again." | the same job |
| `MidiNotBack` | written and verified | "Installed, but the FM-1 didn't come back as MIDI." | "Switch it off and on." | none |
| Not ready (store or blob) | yes | "This board's store is unreadable" or "The loader file is missing." | "Needs a fix from the Mac." | none |
| Anything else | per `danger()` | `failure_text()` | "Try again." | the same job |

## What a backup is (the explainer's facts)

- **Reads:** all 1 MiB of the FM-1's flash, twice, compared (safety rule 3). Writes nothing to the FM-1.
- **Stored:** in this board's internal LittleFS store, under the unit ID (a hash of the FM-1's Bluetooth sector at
  `0xE9000`).
- **Kept:** the unit's first backup (`baseline_backup`) forever, plus its latest. A new latest replaces the old one
  once it's stored (`Library::add_backup`). Laptop copies come with the Web UI (3b).
- **What it's for:**
  - **The undo:** no write without a verified backup of this unit; Transporter plus backup recovers a brick.
  - **"Running":** the board reads the installed firmware from the latest backup.
  - **Planning:** an install writes only the sectors that differ from it.
- **Not a save state.** A save is one firmware's user data, autosaved before an install switches firmware. Keep the
  two words apart on screen.
- **Names:** "b50", because the store has no clock (decision 5).

## What the code needs

- **`UiLoader` splits by page:**
  - **FM-1 summary:** running label, source backup, latest backup id, library count.
  - **The list:** items with a note kind (running, save, not yet).
  - **The plan:** the install preview's sentences.
  - **The job:** kind, step list with states, percent, danger.
  - **The result:** kind, a safety state (nothing written, written and verified, danger), headline, why, todo,
    detail, and the fix hold's label and action.
  - Today's `lines[6]` diagnostics move to the Web UI's `/status` (and a Details page if Chad wants it).
- **`JobBoot`'s result** carries those result fields instead of a title and six lines. RTC slow memory has room
  (8 KB).
- **A failure-to-message table** in `loader_app.cpp` (the table above), tested like `status.h`'s problem lines.
- **Mode:** `ui_set_mode(Play | Load | Safe)` and `ui_set_fm1_line(...)` feed the strip.
- **Shell:** a tab bar and a sub-page stack. Holds become one shell widget that takes its label and action.
- **`ui_shots`** gains the new pages and drops the old Firmware states.

## Other boards

- **Layout:** the strip, the tab bar and the pinned holds use flex and the `BoardProfile`.
- **Landscape (800×480):** the tab bar can become a left rail.
- **Round boards:** they need the tab bar inset further, and the circle's `safe_padding` handles it.
- **The 1.47" board, with buttons and no touch:**
  - The tabs, rows and back row join an LVGL input group in order.
  - A short press moves focus; a long press selects.
  - Holds stay holds: press and keep holding.

## Decisions for Chad

1. **Tab bar or swipe?** Recommended: the tab bar (39 px), with swipe as a shortcut.
2. **Computer mode: tap-twice or a hold?** Recommended: a hold, on its own page.
3. **Open the result page after a job?** Recommended: yes, with OK going to FM-1 home.
4. **Keep favorites?** Recommended: drop them until the library outgrows a screen.
5. **Backup names: b50 or a time?** Recommended: b50 now, and a time once Wi-Fi gives one.

## Not mocked yet

- Computer mode's screen (unchanged).
- Safe mode's strip and screen.
- The not-ready state.
- Naming a unit (A or B) the first time it's seen.
