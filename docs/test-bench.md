# FM-1 test bench

Firmware switches run from the Mac against FM-1-2 (Chad's second FM-1), with nobody at the desk. Design and reasons:
`fw-map-bench.md` section B (side-session notes). Approved by Chad 2026-11-01 (tracker `asks/fw-map-bench`).

## Hardware

| Part | Role |
|---|---|
| Waveshare-1 (MAC `d8:3b:da:a0:67:d0`) or Waveshare-2 (`a0:85:e3:f0:a2:3c`) | runs the BENCH build; host to FM-1-2 through the hub |
| FM-1-2 (Chad's second FM-1) | the only FM-1 the bench switches or writes. Never FM-1-1, Chad's main FM-1 (unit `69bb070aaa87bd10`) |
| FM-1 Transporter (XIAO RP2040, Quixotic7/MvaveFM1Unbricker @ `707470a`) | FM-1-2's pristine dump; recovery when the FM-1 can't take the SysEx key |

Board revisions (2026-11-01): Waveshare-1 is a V2 board (quiet). Waveshare-2 has no "V2" sticker on its USB port,
so it's a V1 board. V1 boards wire the buzzer to GPIO33, an octal-PSRAM line, so Waveshare-2 may tick in time with
memory traffic. That's harmless, and only cutting or removing the buzzer would cure it. (A test flash of master with the
GPIO42 fix will tell whether its ticking was firmware or hardware.)

Test sessions may erase either Waveshare. The build answers `/bench/` only on one of them, and `tools/bench.py`
checks the board's MAC matches the Waveshare it was asked for (`--board`, default `waveshare-1`). Every script
refuses FM-1-1 by unit ID.

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

Both dump first and refuse FM-1-1. `--mock DUMP` uses the Unbricker's in-process mock.

## Not built yet

- The bench-only sector writer, for test marks, the Skiff fill of 0xEA000–0xFBFFF, and the USR restore from the
  pristine dump. It's guarded tier, so it waits on Chad's go-ahead (`asks/bench-writer-plan`).
- The relay board that hands the FM-1's port to the Transporter (`asks/bench-parts`).
