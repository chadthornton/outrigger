# UI redesign: nav, Play, switching firmware, errors

**Status:** design agreed with Chad over five rounds of review, 2026-10-30. Mockups only; nothing is in `host_ui`
yet. Build it after `loader-session3` merges: ui-polish rewrote `host_ui`, and that merge carries the current
Firmware page (list → detail, `154d147`).

- **Review page:** https://claude.ai/artifact/AemH6yE3Nzyeu6GYobHaeg. It shows every screen on every board and
  text size.
- **Mockups:** `make ui-mocks` renders them with LVGL from `tests/ui_mocks/ui_mocks.cpp` into `build/ui-mocks/nav/`.
  - **Coverage:** 240×280 rounded, 320×480 upright and 480×320 sideways, each at S, M and L.
  - **Checks:** the same label checks as `make ui-shots`: 0 problems. Each shot's "fits" or "scrolls" is in
    `nav/report.txt`.
  - **Sample data only.** The mock code is a layout reference, not code to merge.
  - **Round 1** (the first pass, every page under a status strip and bottom tabs) still renders to
    `build/ui-mocks/` for comparison. It's superseded.

**Chad's goals:** "a top level nav and better error messaging and better clarity around what's currently running
and what the backup process is". Then, in review: switching firmware is the tool's main job and shouldn't be
cumbersome.

## Screen map

```
Play (4 lines, tap to open)
FM-1 (firmware list) ── Switch to X (plan + hold) ── Switching (job) ──┬── Switched
   │                                                                 └── errors (safe, or retry)
   └── Backups (hold: back up now) ── Backing up (job)
Setup ── Computer mode (hold) ── Computer mode screen
```

- **Top-level sections** (Play, FM-1, Setup) carry the nav.
- **Sub-pages** have a back row instead: tapping it goes back.
- **Job pages** (switching, backing up, a stopped write) have neither. Routing is off in a job's boot, and there's
  nowhere to go.
- **After a job** the board restarts into Play and opens the result page; its OK returns to the FM-1 list.

## The nav is the header

- **No status strip.** The nav is the only chrome on a top-level page.
  - **Portrait:** a full-width band of three icons from the top edge of the glass into its corners. The band is
    the first thing in the page's scroll, so on a page taller than the screen it scrolls away with the content.
  - **Landscape:** a rail down the left edge, top to bottom, fixed while the page scrolls beside it.
- **Icons:**
  - **Play:** a 5-pin MIDI DIN socket.
  - **FM-1:** a cartridge (a wide shell with a label, a narrower connector with contacts).
  - **Setup:** the gear (`LV_SYMBOL_SETTINGS`).
  - **Size:** 24 px on the 1.69", 30 px on 320+.
  - The mock draws them from LVGL shapes (`icon_midi`, `icon_cart`); build them as a small A8 image or icon-font
    set.
- **Active section:** the icon in white, with a blue bar under it (beside it on the rail). The others are grey.
- **Attention dots** show from any section:
  - **Orange on Play:** the chain has a problem.
  - **Red on FM-1:** an install is unfinished.
- **What the strip used to carry:**
  - **The firmware:** the FM-1 line on Play shows it, and its detail says where that came from.
  - **The mode:** PLAY is the normal state and needs no label; a job's LOAD boot takes the whole screen.

## Play: one line per stage

- **Four lines:** Hub, Controllers, Router, FM-1. Each has a dot, a name and one status on the right.
- **Status:** terse, worst thing first.
  - **Hub:** "USB 8/8" (yellow when full).
  - **Controllers:** "LAU playing" (green while messages arrive), or "6, 1 issue" (orange).
  - **Router:** "123k sent".
  - **FM-1:** "Skiff 1.4", or "no answer" (orange).
- **Tap a line** to open that stage's detail, which is today's lines, wrapping rather than cut. One stage is open
  at a time, and a second tap closes it.
- **A problem opens its own stage** with the fix card ("FM-1 not answering / Replug it into hub port 2") and puts
  the orange dot on Play.
- **Sizes:**
  - **Lines:** 36 px tall on the 1.69", 44 px on 320+, so each is a real hit target.
  - **The 1.69":** no chevron, and at L the status drops under the name.
- **Fit:** collapsed, Play fits at S and M on every board.
  - **Open stages:** with six controllers open, the page scrolls.
  - **The fit loop:** on boards 320 px and wider it only tightens gaps, then scrolls. The 1.69" keeps today's
    shrink-to-fit.
- **Code:** each stage needs a status line from `status.h` (its worst condition, else the latest activity) next to
  the existing detail lines.

## FM-1: the firmware list, and switching

**The list.** The FM-1 section opens on the firmware list:
- **The running one** is first, in green, with a play mark.
- **"your songs"** marks a firmware that has a save state for this unit; switching to it brings them back.
- **"not yet"** marks SLOOP, until its data map is known.
- **Bottom:** a quiet "Backups" row.
- **No favorites:** the whole list fits.

**Where "running" comes from.** `identify_app()` over the unit's latest backup. Skiff calls itself Felucca on USB,
so the USB name is never used.

**Switching.** Tapping a firmware opens its plan, then one hold, "Hold: switch to Felucca 1.6.1". The plan:
- "Switches from Skiff 1.4."
- "Backs up only if the FM-1 changed."
- "Saves Skiff's songs."
- "Writes Felucca: 143 sectors."
- "Brings back Felucca's songs", or "Starts Felucca fresh: no songs saved yet".
- "MIDI stops until it's done."

**What the hold runs (Chad's decision: back up only when the FM-1 changed):**
1. **The install visit, as today:** autosave from the stored backup, plan, key, then one read compared with the
   backup (`Installer::do_check`).
2. **If it matches:** write, restore that firmware's songs or start fresh, then restart the FM-1.
3. **If it doesn't (`BackupStale`):** nothing has been erased.
   - The job records "switch, backup needed" in `JobBoot`.
   - It restarts into a backup boot (two reads, compared; the result becomes the latest backup).
   - Then it restarts into an install boot and goes back to step 1 against the fresh backup.
4. **A second `BackupStale`** right after the fresh backup stops the switch with "The FM-1 changed again right
   after its backup". It doesn't loop.

**Why this keeps songs safe.** The autosave comes from the stored backup, and the check proves that backup equals
the FM-1 before any erase. So a changed FM-1 always gets a fresh backup, and a fresh autosave, before anything is
written.

**Why the backup and the write can't share a visit.**
- The FM-1 left ROM boot about 8.5 s into a visit that read twice and then planned (checkpoint 3).
- `kLoaderBudgetMs` (6 s) caps the time before the first erase.

**Cost.** None when nothing changed. When it changed, one stopped visit plus a backup, estimated at about 30 s
more (the plan's figure for a manual save). Felucca's autosave may make "changed" the usual case: measure it once
built, and revisit "always back up first" if so.

**Guarded.** This changes the install path: one hold runs up to three ROM-boot visits. It needs its own plan,
Chad's go-ahead and tests first in the loader session. Tests to write first:
- **Stale switch:** a stale check, then a backup, then an install that succeeds, with the songs saved from the new
  backup.
- **Stale twice:** a second stale check stops the switch.
- **Unchanged FM-1:** nothing is backed up.

**Backups page.** Reached from the quiet row at the bottom of the list.
- **What it says:** a switch backs up by itself when the FM-1 changed, so you don't need to.
- **What a backup is:** it reads all 1 MB twice and keeps it on this board, and writes nothing to the FM-1.
- **What's kept:** the first (b1) and the latest (b50).
- **The hold:** "Hold: back up now", for before something risky or (3b) to copy one to a computer.
- **Why not Setup:** backups belong to one FM-1; Setup is about this board.

**An unfinished install overrides the list.** When `resume_pending()` is true (the board booted with the FM-1
still in ROM boot and a pending record):
- **The FM-1 page:** "Install unfinished", with a red "Don't unplug the FM-1 or switch it off" card and the retry
  hold as its only action. The list and Backups are gone.
- **The nav:** a red dot on FM-1.
- **On Play:** the FM-1 line reads it too.

This is checkpoint 3's mix-up, where a hold labelled for one job ran another. The hold's label always names what
it runs.

## Holds

- **What needs one:** anything that touches the FM-1 or stops MIDI is a 2-second hold. That covers a switch, a
  manual backup, a retry and Computer mode.
- **The label** names exactly what it runs: "Hold: switch to Felucca 1.6.1", "Hold: retry Felucca 1.6.1".
- **Position:** pinned under the scrolling page, so it never scrolls away.
- **On the 1.69"** the label is 12 px, so it stays on one line.

## Job pages

- **A title, then steps:** a check for done, an arrow and live text for the current step, a dash for steps to come.
  Then a progress bar.
- **No nav and no back arrow.**
- **Switch, nothing changed:** "Checked: same as b50", "Saved Skiff's songs", "Writing 88 of 149", "Restart the
  FM-1".
- **Switch, the FM-1 changed:** "Changed since b50: nothing written", "Backing up: 2nd read, 42%", then the rest.
- **Manual backup:** "FM-1 in ROM boot", "Reading, 2nd pass", "Compare and store", "Restart the FM-1", then
  "Writes nothing to the FM-1."
- **The red "Don't unplug or switch off the FM-1" card** shows only while `Installer::danger()` is true.
- **Step mapping:** the steps group `Step` values: SendKey/WaitRom; Identify/UploadLoader/ReadIds; Backup or
  Plan/ResumeRead; WriteApp/WriteData/Check; Reset/WaitMidi.

## Results and errors

**The pattern.** The page's title is the headline: orange when safe, red when not. Under it, in order:
1. **A safety card,** in one of three states:
   - **Nothing written** (green): "Nothing was written."
   - **Written and checked** (green): "Installed and checked." The error came after, as in `MidiNotBack`.
   - **Danger** (red): "Don't unplug the FM-1 or switch it off." `danger()` is true.
   - **Which state:** whether the job wrote anything (sectors written, or a write step reached), not `!danger()`
     alone.
2. **Why:** one plain sentence.
3. **What to do:** one sentence.
4. **Detail:** small grey text, for us: `failure_detail()`, `scsi.last_failure`, `host.why`.
5. **A pinned hold** that runs the fix and names it, or OK when there's nothing to run.

**Switched**, the success page: "Felucca 1.6.1 is running", then the steps done, then OK.

**Mapping from today's signals:**

| Signal | Safety | Why | To do | Hold |
|---|---|---|---|---|
| `danger()` after `DeviceLost`, `WriteVerifyFailed` | danger | "It's in ROM boot with half of X written." | "Hold to retry: it carries on where it stopped." Detail adds "If the retry fails: Transporter + bN." | retry X |
| `KeyNotSent`, `host.why` "FM-1 already in ROM boot", no pending record | nothing written | "The FM-1 was already in ROM boot." | "Switch the FM-1 off and on, then try again." | switch to X |
| The same, with a pending record | danger | Route to "Install unfinished". Never advise a power cycle. | | retry X |
| `KeyNotSent`, "no FM-1 on the hub" | nothing written | "No FM-1 on the hub." | "Plug it into hub port 2." | OK |
| `NoRomDevice` after 3 tries | nothing written | "The FM-1 took the key but didn't show up in ROM boot." | "Switch it off and on, then try again." | switch to X |
| `BackupStale` a second time | nothing written | "The FM-1 changed again right after its backup." | "Leave it untouched for a moment, then try again." | switch to X |
| `BackupMismatch` | nothing written | "The two reads differed." | "Try again. If it repeats, check the cable and hub." | the same job |
| `LoaderTooSlow` | nothing written | "The FM-1 was too slow to read." | "Try again." | the same job |
| `MidiNotBack` | written and checked | "Installed, but the FM-1 didn't come back as MIDI." | "Switch it off and on." | OK |
| Not ready (store or blob) | nothing written | "This board's firmware store can't be read" / "The loader file is missing." | "It needs a fix from a computer. MIDI still works." | OK |
| Anything else | per the rule above | `failure_text()` | "Try again." | the same job |

The first `BackupStale` isn't an error any more: the switch backs up and carries on. "Back up first"
(`NoBackupForInstall`) can't happen either, because a switch with no backup backs up first.

**Chain problems** stay on Play, opening their own stage.

## Setup and Computer mode

- **Setup:**
  - The board's version and Web address.
  - Text size S/M/L.
  - "Rotate screen" on boards that can turn.
  - "Computer mode".
- **Computer mode** gets its own page, explaining that it restarts with MIDI off so a computer can update the
  board, and "Hold: restart in Computer mode". This replaces today's tap-twice.
- **The Computer mode screen** has no nav, since the USB host isn't running. It shows the instructions, the
  version and the Web address, and one "Back to MIDI" button.

## Screen shapes

The layout follows the screen's shape; a new board needs only a `BoardProfile`. Every board uses one column. A
side-by-side landscape layout was tried and dropped: it read as two unrelated lists. A narrow side column may come
later, but only for detail about the row directly to its left.

| Shape | Boards | Nav |
|---|---|---|
| Small portrait (under 400 px tall) | 240×280 (this one), 240×320, 172×320 | top band |
| Tall portrait | 320×480 upright | top band |
| Landscape | 480×320, 800×480 | left rail |
| Square or round | 480×480, 412×412 | top band, icons kept inside the circle |

- **Chad's two new panels** are 320×480 (IPS and SPI): 2:3, a bit taller than 4:3.
- **Recommended defaults:** landscape on the 320×480 panels, portrait on the 1.69". Rotation is an LVGL display
  rotation, saved in settings.
- **Square-cornered panels** use a 10 px side margin and 8 px top and bottom (host_ui's rect padding is 6).
- **The 3.5" panels** get `font_boost` 1.

## Input without touch

Not every ESP32 board has touch. Many have one to three buttons, a rotary encoder or a 5-way switch; the 1.47"
Waveshare is one. Every screen works by focus too, through an LVGL input group in reading order: the nav icons,
then each line on the page.

| Input | Move | Open | Hold |
|---|---|---|---|
| Touch | scroll | tap | keep pressing 2 s |
| Encoder | turn | press | keep pressing 2 s |
| 5-way | up, down; left, right between sections | center | hold center 2 s |
| Two buttons | short A | short B | hold B 2 s |
| One button | short press | long press | keep holding to 2 s |

- **The focus ring** is a 2 px blue border on a line, or an outline on a nav icon, so nothing moves when focus
  arrives. It shows only on boards without touch.
- **The `BoardProfile`** gains the input kind (touch, encoder, keys) and its pins.

## What a backup is

- **Reads:** all 1 MiB of the FM-1's flash, twice, compared (safety rule 3). Writes nothing to the FM-1.
- **Stored:** in this board's internal LittleFS store, under the unit ID (a hash of the FM-1's Bluetooth sector at
  `0xE9000`).
- **Kept:** the unit's first backup (`baseline_backup`) forever, plus its latest. `Library::add_backup` replaces
  the old latest once the new one is stored, and unchanged sectors are stored once. Laptop copies come with the
  Web UI (3b).
- **What it's for:**
  - **The undo:** no write without a current backup; Transporter plus backup recovers a brick.
  - **"Running":** the installed firmware is read from the latest backup.
  - **Planning:** a switch writes only the sectors that differ.
  - **Songs:** a switch saves the running firmware's songs from it.
- **Not a save state.** A save is one firmware's songs and settings. Keep the two words apart on screen.
- **Names:** "b50", because the board has no clock. Times can come once Wi-Fi gives one.

## What the code needs

- **Shell (`host_ui`):**
  - The nav band and rail.
  - A sub-page stack with back rows.
  - Job pages.
  - One pinned-hold widget that takes its label and action.
  - An input group for boards without touch.
  - Layout by screen shape from the `BoardProfile`.
- **`UiLoader` splits by page:**
  - **The list:** items with a note kind (running, your songs, not yet).
  - **The plan:** the preview's sentences.
  - **The job:** kind, steps with states, percent, danger.
  - **The result:** a safety state, headline, why, to do, detail, and the fix hold's label and action.
  - Today's `lines[6]` diagnostics move to the Web UI's `/status`.
- **`JobBoot`:**
  - **A switch job kind,** which can chain backup → install.
  - **A structured result** in place of a title and six lines. RTC memory has room.
- **A failure-to-message table** in `loader_app.cpp` (the table above), tested like `status.h`'s problem lines.
- **`status.h`:** a one-line status per stage for Play.
- **`ui_shots`:** gains the new pages and drops the old Firmware states.

## Open for Chad

1. **Icons:** do the MIDI socket and the cartridge read right? (Recommended: keep both.)
2. **Panel chips:** which display driver and touch controller do the 320×480 panels have? The board profiles need
   them.
3. **Recommended defaults** to confirm when building:
   - The nav scrolls away in portrait (Chad's proposal).
   - Landscape by default on the 320×480 panels.
   - Computer mode is a hold.
   - The result page opens after a job.

## Not mocked

- Safe mode after a crash.
- Naming a second FM-1 the first time it's seen.
- The 1.47" button board. Its focus order is described above.
