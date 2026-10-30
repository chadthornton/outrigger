# How robustness was earned

[Index](index.md) · previous: [The loader's safety design](loader-safety.md) · next: [Mistakes, and how each was caught](mistakes.md)

AI coding sessions (Claude) wrote nearly all of this project's code between 2026-10-26 and 2026-10-30, across many
sessions, often in parallel, each in its own git worktree. Chad decided what to build, approved the plans for risky work,
and did everything physical at the board. He doesn't check the code himself. So the question this page answers is practical:
what stood between an AI's confident claim and a bricked FM-1?

Short answer: tests that compare against the proven reference tools, a lock that kept writes off until hardware
proved each step, checkpoints at the board with pass criteria written in advance, soak runs, and one review round per
risky branch. None of it was enough alone. [Mistakes](mistakes.md) lists what each layer caught and what got
through.

## The rules the sessions worked under

- **Two tiers.** "Guarded" work is anything a rollback can't undo: writing the FM-1's flash, the partition table,
  stored data. "Light" work is everything else (UI, layout, read paths).
- **Guarded work needs a plan Chad approves,** is written test-first, and gets one `code-review` round at high
  effort before it merges. Light work skips the review and gets checked on the board or in screenshots.
- **No "done", "fixed" or "passing" without test output from the same session.**
- **The merge gate** (README): Mac tests pass, CI is green, and a hardware smoke check passes on the branch's own
  build: install it with the web installer, rig all green at 8/8, Launchkey plays the FM-1.
- **One board, booked.** There's one Waveshare. A session books it in a shared notes queue before asking Chad to
  move it, and reflashes `master` when done.

## Layer 1: Mac tests

`make test` builds and runs 15 test binaries (from 14 source files; the library suite is built twice) with the Mac's clang, `-Wall -Wextra -Werror`, no hardware. All of the
plain-C++ libraries are tested this way: `midi_core`, `platform`, `fm1_loader`, `web_core`.

| Suite | Test functions | Covers |
|---|---|---|
| `test_fm1_installer` | 30 | The install state machine against a mock ROM with NOR semantics: installs, no-ops, retries, resume, disconnects, failure detail. |
| `test_broker` | 22 | Claims, channel cost, modes, FM-1 ownership, every screen reason fits 33 characters. |
| `test_fm1_library` | 13 | Units, backups, saves, data decisions, planners. Built twice: against `MemoryStorage` and `PosixStorage`. |
| `test_web_core` | 10 | Log rings, routes, the Host guard, the hotspot channel. |
| `test_midi_core`, `test_status` | 9 each | Routing (system messages never reach the FM-1), chain health. |
| `test_fm1_package` | 7 | Real packages, damaged copies, every guard edge. |
| `test_boot_mode`, `test_describe`, `test_fm1_posix`, `test_fm1_softkey` | 4–5 each | Computer mode, descriptor parsing, atomic writes, key entry. |
| `test_fm1_locked` | | A whole install in a build with writes locked: no erase or write command is ever sent. |
| `test_fm1_interop`, `test_store_json` | | Cross-language checks (below). |

On 2026-10-30, at `ff919c5`, every suite passed (run while writing this page).

**What the Mac tests can't see:** every failure on [the USB failures page](usb-host-failures.md). Stack depth,
critical sections, ESP-IDF's host driver and the FM-1's timing don't exist on the Mac.

## Layer 2: golden vectors from the proven tools

The ROM-boot sequence wasn't re-derived; it was ported from Python that had already recovered a bricked FM-1.

- **The reference:** Quixotic7's MvaveFM1Unbricker (`fm1_unbrick.py`, pinned at its own repository's commit `707470a`), driving kagaimiq's
  jl-uboot-tool classes (`JL_UBOOT`, `JL_LoaderV2`).
- **How:** `make golden` (`tools/gen_golden.py`) runs the Unbricker's own functions against the Unbricker's own
  CDB-level `MockScsi`. Each scenario file in `tests/golden/` lists every SCSI command the host sends (CDB,
  direction, length, hash of the data out) and ends with a hash of the mock's final flash.
- **The check:** the C++ state machine, against a C++ port of that mock, must reproduce each file exactly.
- **Ten scenarios:** install, no-op, a retry that clears, one that doesn't, wrong chip key, wrong flash ID, backup
  reads that disagree, resume, a stale backup, and a backup job. Plus package and cipher vectors.
- **Where the C++ differs on purpose,** the generator composes the Unbricker's own functions to match: it writes
  only differing 4 KiB sectors with the Unbricker's erase, write and read; it skips the diagnostic RAM read; it
  resets with jl-uboot-tool's `run_app(1)`.
- **They move with the design.** When `d860144` changed the install to read once, the vectors were regenerated from
  the reference, not edited by hand.

What it caught: the reset command. The first plan used `0xFC02`; porting from jl-uboot-tool showed that's the V1
loader's reset and the wl82 runs V2, so it's `0xFC0C` with argument 1. Checkpoint 1 confirmed it on the FM-1.

## Layer 3: interop tests

Two languages write the same store. `tools/fm1lib.py` adds packages to the board's store from the Mac
(`make store-add`), and the board reads them with `lib/fm1_loader`.
- `make test` runs `fm1lib.py selftest`, which writes Skiff 1.4, SLOOP 2.8.1 and stock V15 into a directory.
- `test_fm1_interop` then reads that directory with the C++ and checks every package against what the C++ makes of
  the same file.
- `test_store_json` writes the `/store` JSON and Python parses it.
- `tools/check_partitions.py` checks the built partition table keeps the install layout.

## Layer 4: screenshots on the Mac

`make ui-shots` (`2b51134`) renders every `host_ui` screen with LVGL on the Mac at three text sizes, taps them with
a finger that drifts 0, 8 and 16 px, and fails on a clipped label, a label outside the round screen's safe area, or
a failed tap. On 2026-10-27 `master` had 79 screens and 0 problems. A deliberately broken layout (negative control)
produced 18 problems and 6 failed taps, so the check can fail. It found Firmware page lines wider than the page at
every size, and later 93 problems when the page grew a package list. CI runs it on every push.

## Layer 5: checkpoints at the board

Each hardware step was written down before it ran, with a pass criterion, and run with Chad present. Writes stayed
locked in firmware builds until checkpoint 2.

| Checkpoint | Date, build | What passed | What it found |
|---|---|---|---|
| 1, read-only backup | 2026-10-27, `e848423` | "Backed up. Nothing written." Boot head and Bluetooth/factory area equal the 2026-10-25 dump. Run twice. | Skiff accepts the key; the library's attach is harmless; `0xFC0C` restarts the FM-1; a too-early send; an intermittent ROM-boot miss. |
| 2, no-op install | 2026-10-27, `b81773c` | Skiff over Skiff: preview 0 sectors, nothing erased, FM-1 came back by itself. | A reboot loop: allocation in a critical section, then a stack overflow. |
| 3, real install | first try 2026-10-27; passed 2026-10-30, `fc72748` | Skiff → Felucca 1.6.1 → Skiff, all 149 sectors verified; a marked Skiff project survived the round trip. | The 8.5 s self-restart; in-place host restarts failing; a mislabelled hold; the post-install record gap. |

**The first checkpoint 3 attempt stopped safely.** Skiff → Felucca stopped mid-write phase with "the FM-1 stopped
answering". The FM-1 restarted on its own. The next key brought up no ROM device, and the FM-1 went dark. After a
power cycle, a backup showed the app area byte for byte Skiff: nothing had been written. The cause is **open**
(suspects: the FM-1's battery under erase current, or a loader or USB fault). What came out of it: failures that
name their command and address, and the web diagnostics.

## Layer 6: soak runs

A fix guessed from one crash proves nothing, and each board trip costs Chad time. So `SOAK=1` builds back up the FM-1
by themselves, job after job, and `tools/soak_watch.py` keeps every log tail and core dump on the Mac.

| Build | Finished runs | OK | No ROM device | Crashes |
|---|---|---|---|---|
| `a616453` | 6 | 3 | 3 | 2 |
| `77ff4ec`, hub tracking off in job boots | 8 | 6 | 2 | 1 |

Counts are from the soak logs; the commit messages and plan disagree with them and with each other (see
[mistakes](mistakes.md#the-soak-counts)). The bar for calling the crash fixed is at least 20 runs with 0 crashes. It
hasn't been met.

## Layer 7: code review

One review round at high effort per guarded branch, by a separate review run. Every finding is fixed or dismissed
with a one-line reason, and the merge commit records the count. As recorded in `git log`:

| Branch | Merge | Review line |
|---|---|---|
| `routing` | `d5dca64` | 0 critical, 2 important (5 fixed, 2 dismissed) |
| `loader-core` (session 1) | `5f17e25` | 0 critical, 10 important (8 fixed, 2 dismissed) |
| `web-installer` | `e2da15a` | 0 critical, 3 important (7 fixed, 1 dismissed) |
| `platform-architecture` | `ea2ac4c` | 0 critical, 8 important (6 fixed, 2 dismissed); `86da5ee` itself says 6 important (5 fixed, 3 dismissed) |
| `loader-session2` | `6a2fcd4` | 0 critical, 7 important (6 fixed, 1 dismissed) |
| `web-diag` | `18ad445` | 0 critical, 10 important (9 fixed, 1 dismissed) |
| `loader-session3`, first round to `d2284ba` | not merged | 9 findings: 7 fixed, 1 no change, 1 skipped (session notes; fixes in `4985af4`) |
| `lvgl`, `ui-chain`, `broker-wiring`, `ui-polish`, `ui-shots` | | light tier, none |

Several lines have more fixed and dismissed findings than critical plus important ones. The difference is probably
lower-severity findings, but the record doesn't say. `broker-wiring` was classed light although it decides every
USB claim; its smoke check passed at 8/8.

**Examples of what review caught:**
- the loader blob wasn't checked before the key, and without it nothing can reset the FM-1 out of ROM boot
  (`c55a581`);
- a hub's interrupt pipe wasn't counted, though the hub driver always opens it (`86da5ee`);
- the retry lived in RAM, so a crash mid-install lost it; it now comes from the store (`4985af4`);
- the hold could install a different package than the preview showed after a pin re-sorted the list (`4985af4`).

**Not reviewed yet:** everything guarded on `loader-session3` since `d2284ba`, which includes the plan-before-key
install, job boots, and the post-install record. The next review round covers it before the merge.

## What to take from this

- **What held:** nothing was ever written to the FM-1 that its backup couldn't restore. Every crash came before an
  erase. No record shows the guard or the lock refusing a write on the board; the tests show they would.
- **The reference tools did the most to keep the port honest.** Comparing every SCSI command against code that had
  already recovered a bricked FM-1 left little room for invention.
- **The board did the most to find bugs.** Every runtime failure in this project was found on hardware, mostly at
  checkpoints, and the crash forensics were built because the first crash had no log.
- **Where the AI went wrong** was mostly in stating confidence ahead of the evidence: "0 crashes" written after a
  crash was on record, soak counts that differ from the logs, a cause named before it was tested. The written-down pass criteria, the soak
  logs and the stale-backup check caught those, not the AI's own judgment.
