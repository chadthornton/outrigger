# FM-1 test bench

Firmware switches run from the Mac against an FM-1 (FM-B by default), with nobody at the desk. Design and reasons:
`fw-map-bench.md` section B (side-session notes). Approved by Chad 2026-11-01 (tracker `asks/fw-map-bench`).

## Hardware

| Part | Role |
|---|---|
| Small-V1, Small-V2 or Big-S3 (`--board`, default Small-V2) | runs the BENCH build; host to the FM-1 through the hub |
| FM-B, or FM-A when a run asks for it (`--fm1`, default FM-B) | the FM-1 the bench switches |
| FM-1 Transporter (XIAO RP2040, Quixotic7/MvaveFM1Unbricker @ `707470a`) | an FM-1's pristine dump; recovery when the FM-1 can't take the SysEx key |

The names, MACs and unit IDs come from the device registry (README, "Boards").

Buzzer (2026-11-01): both Waveshares ticked or chattered while GPIO42, the buzzer pin in Waveshare's pin map, was left
floating. Master since PR #30 holds it low, and Small-V1 (no "V2" sticker) went silent on it. So the sticker doesn't
predict the buzzer pin, at least on this unit.

Test sessions may erase any of these (Chad, 2026-11-03). The build answers `/bench/` only on Chad's boards
(`lib/web_core/src/bench.cpp`), and `tools/bench.py` checks the board's MAC and the attached FM-1's unit ID are the
ones `--board` and `--fm1` name. That checks the run has the devices it asked for; none is off limits.

## Using the lab board

Small-V2 runs `hub-loader` with `DIAG_WEB=1 BENCH=1` and stays on Chad's home Wi-Fi at `fm1bench.local`, with FM-B
on its own powered hub (the rig hub keeps Small-V1 and FM-A). Any session on this Mac can read it instead of asking
Chad what the screen shows.

**Reads are free.** No booking; nothing changes on the board or the FM-1:

```bash
python3 tools/bench.py fm1                          # FM-1 on USB?, which FM-1, running firmware, last result, latest backup, build
python3 tools/diag.py --host fm1bench.local status  # build, heap, tasks, USB devices, the Firmware page as the screen says it
python3 tools/diag.py --host fm1bench.local log --follow
python3 tools/diag.py --host fm1bench.local crash   # the last abnormal reset, and whether a core dump is stored
python3 tools/diag.py --host fm1bench.local all     # everything into build/diag/<time>/
```

**Switches and backups book the board first**, through the orchestrator, like the Mac's USB slot. `bench.py` also
takes a lock shared by every worktree (`~/Library/Application Support/outrigger/bench.lock`), so a second run on this
Mac fails at once instead of interleaving:

```bash
python3 tools/bench.py backup                  # reads FM-B; writes only the board's own store
python3 tools/bench.py switch "Skiff 2.1.1"    # backup (identifies the FM-1), plan check, hold, back on USB for 60 s, backup
```

A switch's JSON line carries `checks`, each read from the board: `result_ok` (the result page says it worked),
`fm1_back` (the FM-1 re-enumerated within 60 s), `fm1_steady` (no USB changes in the next 60 s), `backup_ok` (the
follow-up backup passed), and `running` (the board identifies the backed-up app as the firmware asked for, by its
hash: `loader_app.cpp`, `known_unit`). The run passes when all five do. A factory-installed stock V15 may not be
recognised as running (issue #39); a stock V15 the loader installed is.

**What still needs Chad's hands** until the relay parts arrive (`asks/lab-board-parts`):
- **Reflashing the lab board.** The build has no over-the-air update and no remote Computer mode, so a new build means
  Chad taps Computer mode and moves its cable to the Mac. Its build lags `master`: check `bench.py fm1`'s `build`
  before trusting a result for a recent change. Wi-Fi survives a reflash: `make flash` writes the bootloader,
  partition table and app but leaves NVS (0x9000) alone; erasing the whole flash or NVS loses it.
- **A dark FM-B.** If a switch leaves it in ROM boot (`unfinished` in the record), recovery is the Transporter, and the
  cable move is a person's.
- **Switching FM-B off and on.** It runs from its own battery (2000 mAh), so cutting USB power doesn't restart it.

If `fm1bench.local` doesn't resolve, the board's IP is on its Setup screen; `--host <ip>` works for both tools. After
a power cut the board rejoins Wi-Fi by itself; if the network never answers, it opens the `fm1bench-setup` hotspot
after 60 s.

## Build and flash

```bash
make build APPS=hub-loader DIAG_WEB=1 BENCH=1     # build/waveshare_169/hub-loader-web-bench
tools/wait_for_board.sh Small-V2                  # the port, once Small-V2 is on the Mac in Computer mode (MAC checked)
make flash APPS=hub-loader DIAG_WEB=1 BENCH=1 PORT=/dev/cu.usbmodemXXXX
```

Always pass `PORT=` from `wait_for_board.sh`: the Transporter's XIAO is also a `/dev/cu.usbmodem*`. The bench answers
at `fm1bench.local`, never `fm1hub.local`, and its setup hotspot is `fm1bench-setup`. `make site` refuses any shipped
build that contains `/bench/`.

## Run

```bash
python3 tools/bench.py state
python3 tools/bench.py switch "Skiff 2.1.1"      # plan check, hold, FM-1 back on USB for 60 s, backup
python3 tools/bench.py backup
python3 tools/bench.py switch "Skiff 2.1.1" --dry-run   # a fake board, no network
```

One JSON line per run goes to `build/bench/runs.jsonl`; a switch passes when all five `checks` do (above).

## Transporter

```bash
python3 tools/bench_recover.py dump --port /dev/cu.usbmodemXXXX          # into the recovery project's backups/
python3 tools/bench_recover.py recover PKG.fwsc --port ... [--write]      # dry run without --write
```

Both dump first and refuse any FM-1 but the one `--fm1` names (default FM-B). `--mock DUMP` uses the Unbricker's
in-process mock.

## Not built yet

- The bench-only sector writer, for test marks, the Skiff fill of 0xEA000–0xFBFFF, and the USR restore from the
  pristine dump. It's guarded tier, so it waits on Chad's go-ahead (`asks/bench-writer-plan`).
- The relay board that hands FM-B's port to the Transporter and the lab board's own port to the Mac
  (`asks/lab-board-parts`).
- fw-map-bench A4's tests 1 and 2 need the writer's marks: without them, a switch run checks only that each firmware
  installs, boots and backs up.
