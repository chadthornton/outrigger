# UI redesign: top-level nav, errors, what's running (draft for Chad's sign-off)

**Status:** proposal, 2026-10-30; round 2 (top level, more screens) round 3 (one column in landscape), round 4
(compact Play, input without touch) and round 5 (switching backs up by itself when the FM-1 changed) the same day. Mockups only, nothing built into `host_ui` yet. Build it after `loader-session3`
merges (ui-polish rewrote `host_ui`, and that merge carries the current Firmware page).

- **Review page:** https://claude.ai/artifact/AemH6yE3Nzyeu6GYobHaeg (every screen at S, M and L).
- **Mockups:** `make ui-mocks` renders them with LVGL from `tests/ui_mocks/ui_mocks.cpp` into `build/ui-mocks/`,
  with the same label checks as `make ui-shots`.
  - **Results:** 45 screens (15 at each size), 0 layout problems. Every page fits at S and M; at L, 12 page bodies
    scroll, but their holds and the tab bar stay in view.
  - **Sample data only.** The mock code is a layout reference, not code to merge into `host_ui`.

**Chad's goals:** "a top level nav and better error messaging and better clarity around what's currently running
and what the backup process is."

## Round 2: the top level (Chad's notes, 2026-10-30)

Chad's notes on round 1: drop the header and let the nav be the header; make it full width, bleeding off the
top of the glass, and let it scroll away with the page; make it icon-driven (a MIDI or routing icon for Play, a
cartridge for FM-1); start with the top level and see how it propagates; and plan for other screens now,
starting with two 320×480 panels (IPS and SPI), where landscape may suit the bigger sizes.

Rendered by `make ui-mocks` into `build/ui-mocks/nav/` on 240×280, 320×480 and 480×320 at S, M and L: 0 layout
problems. One detail line is cut with "..." (320×480 L, six controllers).
- **The nav is the header.** The status strip is gone.
  - **Portrait:** a band of three icons, full width, from the top edge of the glass into its corners. It is the
    first thing in the page, so on a page taller than the screen it scrolls away with the content.
  - **Landscape:** a rail down the left edge, top to bottom, fixed while the page scrolls.
  - **Active section:** the icon in white with a blue bar under it (beside it on the rail); the others grey.
  - **Attention badges:** a dot on the icon. Orange on Play when the chain has a problem; red on FM-1 when an
    install is unfinished. They show from any section.
- **Icons**, drawn from LVGL shapes in the mock (`icon_midi`, `icon_cart`): Play is a 5-pin MIDI DIN socket, FM-1
  is a cartridge (a shell with a label, a connector tab with contacts), and Setup is the gear. 24 px on 240×280, 30
  px on 320+. The real ones would be a small A8 image or icon-font set at those sizes.
- **What the strip carried, now:** the chain's FM-1 node reads "runs Skiff 1.4", and FM-1 home gives the source.
  PLAY needs no label (it's the normal state); a job's LOAD boot still takes the whole screen.
- **Holds stay pinned** below the scrolling page, also on top-level pages (the unfinished-install retry).
- **Round 1's sub-pages** keep the old strip and tabs until Chad approves round 2; then they take the back-arrow
  title row under the same rule (no strip).

### Round 4: compact Play, and input without touch

**Play collapses to one line per stage** (Chad: "a lot of incidental information"). Each of Hub, Controllers,
Router and FM-1 is one line: dot, name, and one status on the right. Tapping the line opens that stage's detail
(today's lines) under it; one stage is open at a time, and a second tap closes it.
- **Statuses** are terse and show the worst thing first:
  - **Hub:** "USB 8/8" (yellow when full).
  - **Controllers:** "LAU playing", or "6, 1 issue" (orange).
  - **Router:** "123k sent".
  - **FM-1:** "Skiff 1.4", or "no answer" (orange).
- **A problem opens its own stage** with the fix card, and badges the Play icon.
- **Lines are 36 px tall on the 1.69" and 44 px on 320+**, so each is a real hit target. On the 1.69" there's no
  chevron, and at L the status drops under the name.
- **Fit:** collapsed, Play fits at S and M on every board, and at L on all but the 1.69" (one line, scrolls). An
  open stage with six controllers scrolls; the 1.69" no longer shrinks text for the chain.
- **Detail lines wrap** instead of being cut with "...".
- The code needs a per-stage status line from `status.h` (the worst condition, else the latest activity) next to
  the existing detail lines.

**Input without touch.** Not every ESP32 board has touch: many have one to three buttons, a rotary encoder or a
5-way switch (the 1.47" Waveshare among them). Every screen works by focus too, through an LVGL input group in
reading order: nav icons, then each line on the page.

| Input | Move | Open | Hold (backup, install) |
|---|---|---|---|
| Touch | scroll | tap | keep pressing 2 s |
| Encoder | turn | press | keep pressing 2 s |
| 5-way | up, down; left, right between sections | center | hold center 2 s |
| Two buttons | short A | short B | hold B 2 s |
| One button | short press | long press | keep holding to 2 s |

- **The focus ring** is a 2 px blue border on the line, or an outline on the nav icon, so nothing moves when focus
  arrives. It shows only on boards without touch.
- **The `BoardProfile`** gains the input kind (touch, encoder, keys) and which pins.

### Round 5: switching firmware backs up by itself

Chad: switching firmware is the tool's main job, so it should be quick, and "is there any reason backing up
shouldn't just happen automatically as part of changing firmware?" There isn't, and the installer needs it.
- **Why a fresh backup matters.** An install autosaves the running firmware's songs from the unit's *stored*
  latest backup, plans from it, then reads the FM-1 once and refuses `BackupStale` if anything differs
  (`Installer::prepare_install`, `do_check`). A song saved after the last backup either blocks the install or,
  without the check, would be lost.
- **Decision (Chad, 2026-10-30): back up only when the FM-1 changed.** The FM-1 section opens on the firmware
  list: the running one first, in green, and "your songs" when a save for that firmware exists. Tapping one
  opens its plan. One hold, "Hold: switch to Felucca 1.6.1", runs:
  1. **The install visit, as today.** Autosave from the stored backup, plan, key, then one read compared with the
     backup (`do_check`).
  2. **If it matches:** write, bring back that firmware's songs or start fresh, restart the FM-1. Done.
  3. **If it doesn't (`BackupStale`):** nothing has been erased. The job records itself as "switch, backup
     needed", restarts into a backup boot (two reads, compared; the new backup becomes the latest), then an
     install boot, which goes back to step 1 against the fresh backup.
- **Songs are safe on both paths.** The autosave comes from the stored backup, and the check proves that backup
  equals the FM-1 before any erase. A changed FM-1 therefore always gets a fresh backup, and a fresh autosave,
  before anything is written.
- **Cost:** none when nothing changed. When it changed, one stopped visit plus a backup, estimated at about 30 s
  more (the plan's figure for a manual save). Felucca's autosave may make "changed" the common case; if so,
  measure it, and revisit "always back up first".
- **Why not one visit:** the backup and the write can't share a visit. The FM-1 left ROM boot about 8.5 s into a
  visit that read twice and then planned (checkpoint 3), and `kLoaderBudgetMs` (6 s) caps the time before the
  first erase.
- **Storage doesn't grow.** `Library::add_backup` keeps the first backup and the latest; unchanged sectors are
  stored once.
- **Manual backup** is a quiet "Backups" row at the bottom of the FM-1 list. Its page explains backups, says what
  is kept, and has "Hold: back up now", for before something risky, or (3b) to copy one to a computer. It stays
  under FM-1, not Setup, because backups belong to one FM-1, and Setup is about the board.
- **Screens in `make ui-mocks`:**
  - `fm1_list`, and `switch_confirm` with and without a save.
  - `switch_writing`: nothing changed.
  - `switch_changed`: "Changed since b50: nothing written", then backing up.
  - `backups`.
  - A running job has no back arrow. Holds on the 1.69" use 12 px text, so the label stays on one line.
- **Guarded.** This changes the install path: one hold can run up to three ROM-boot visits, chained through
  `JobBoot`. It needs its own plan, Chad's go-ahead and tests first in the loader session.
  - **Tests to write first:** stale check → backup → install succeeds, with the songs saved from the new backup.
  - **A second `BackupStale`** after the fresh backup stops the switch with an error, rather than looping.
  - **Round 1's error goes away:** "Install not started / back up again" is no longer shown.

### Screen shapes

The layout follows the screen's shape; a new board needs only a `BoardProfile`.

| Shape | Boards | Nav | Chain |
|---|---|---|---|
| Small portrait (under 400 px tall) | 240×280, 240×320, 172×320 | top band | stacked, shrinks to fit |
| Tall portrait | 320×480 upright | top band | one column, 6 controllers fit at M |
| Landscape | 480×320, 800×480 | left rail | one column beside the rail, scrolls |
| Square or round | 480×480, 412×412 | top band inside the circle | stacked |

- **320×480 is 2:3**, a bit taller than 4:3.
- **Recommended defaults:** landscape on the 320×480 panels, portrait on the 1.69". Setup gets a "Rotate screen"
  row on boards that can turn (an LVGL display rotation, saved in settings).
- **Square panels** use a 10 px side margin and 8 px top and bottom (host_ui's rect padding is 6).
- **One column on every board (round 3, Chad):** landscape keeps the rail but stacks each page exactly as portrait
  does. A left-to-right chain and two-column FM-1 and Setup pages were tried in round 2 and dropped: side by side,
  they read as two unrelated lists.
  - **The rail stays put** while the page scrolls beside it. In portrait the band scrolls away with the page.
  - **Text size holds on bigger boards.** On 320 px and wider, the chain's fit loop only tightens gaps, then the
    page scrolls. The 1.69" keeps today's shrink-to-fit.
  - **A narrow side column** is allowed later only for detail about the row directly to its left (e.g. a node's
    counts), never for a second list.
- **Text size steps** get `font_boost` 1 on the 3.5" panels.

### Round 2 questions for Chad

1. Do the MIDI socket and the cartridge read right? (Recommended: keep both.)
2. Should the nav scroll away, or stay put? (Recommended: scroll away, as proposed. It only happens at L on the
   1.69".)
3. Should the 320×480 panels default to landscape? (Recommended: yes, with Rotate screen in Setup.)
4. Which display driver and touch controller do the new panels have? (Needed for their board profiles.)

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
