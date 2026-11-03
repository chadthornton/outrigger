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

## Build and flash

```bash
make build APPS=hub-loader DIAG_WEB=1 BENCH=1     # build/waveshare_169/hub-loader-web-bench
make flash APPS=hub-loader DIAG_WEB=1 BENCH=1 PORT=/dev/cu.usbmodemXXXX
```

Always pass `PORT=`: the Transporter's XIAO is also a `/dev/cu.usbmodem*`. The bench answers at `fm1bench.local`,
never `fm1hub.local`. `make site` refuses any shipped build that contains `/bench/`.

## Run

```bash
python3 tools/bench.py state
python3 tools/bench.py switch "Skiff 2.1.1"      # plan check, hold, FM-1 back on USB for 60 s, backup
python3 tools/bench.py backup
python3 tools/bench.py switch "Skiff 2.1.1" --dry-run   # a fake board, no network
```

One JSON line per run goes to `build/bench/runs.jsonl`. A switch passes when the board's result is ok, the board
lists the new firmware as running, the FM-1 re-enumerates without dropping off for 60 s, and the follow-up backup
passes.

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
- The relay board that hands the FM-1's port to the Transporter (`asks/bench-parts`).
