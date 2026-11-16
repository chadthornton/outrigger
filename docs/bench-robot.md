# Bench robots

A bench robot is a spare ESP32-S3 that plays a USB MIDI controller and listens like a USB MIDI synth, so an overnight
run can count every message the P4 hub passes. This is milestone E2 of the P4 platform spike (`spike-charter-fm1-p4.md`
in the side-session notes). Night 0 (2026-11-15) built it without hardware: it compiles, and the protocol and checker
pass their tests, but it has never been flashed.

| Part | What it is |
|---|---|
| `firmware/bench_robot/` | the firmware: `robot_main.cpp` is the board, `robot_core.cpp` is the protocol and schedule (no Arduino) |
| `tools/robot.py` | drives robots from the Mac: runs a scenario, collects the logs, checks them |
| `tools/robot_scenarios/` | one scenario per hub use case in the charter |
| `tests/test_robot_proto.cpp` | the protocol core on the Mac (`make test`) |
| `tests/robot_sim.cpp` | the same core, several robots behind a pretend P4, in virtual time (`robot.py run ... --sim`) |
| `tests/robot_check_ref.py` | night 0's checker and `expand()`, the reference the tests hold the windowed ones to |
| `tests/test_robot.py` | the checker on made-up logs, and whole scenarios against the simulator with faults injected |

## The board

The target is an Espressif ESP32-S3-DevKitC-1, which has two USB-C ports:

- **USB** (native, GPIO19/20) goes to the P4's hub. It's a USB MIDI device with **two virtual cables**, each with an
  IN and an OUT jack, so one robot can be a controller on cable 1 and a synth on cable 2 at once. The USB serial
  number is the robot's MAC in hex, so two robots on one hub can be told apart.
- **UART** (the board's USB-UART bridge to UART0) goes to the Mac: flashing, and the control line at 921600 baud.

`make bench-robot` builds it (FQBN `esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=default`). The firmware refuses to
compile with the other USB mode. `make flash-bench-robot ROBOT_PORT=/dev/cu.... ROBOT_MAC=aa:bb:...` uploads through
the UART port. It never uses `PORT`, which defaults to the first `/dev/cu.usbmodem*` and so to Big-P4. Before the
upload, `robot.py check-mac` reads the MAC on `ROBOT_PORT` with esptool as an ESP32-S3 and stops unless it's the one
named, so a P4 fails the chip check and a different S3 fails the MAC check. On DevKitC v1.1 boards (with a CH343
bridge) the UART port can also show up as `usbmodem`, which is why the guard checks the MAC rather than the port name.

Night-0 build: 374,718 bytes of flash (28%); 235,272 bytes of RAM in globals (71%), leaving 92,408. Most of that is
the schedule (2,048 events) and the two logs (4,096 records each), in internal RAM, so no PSRAM build option is
needed. They're buffers, not run limits: `robot.py` streams the schedule in and drains the logs as a run goes.

## The line protocol

The protocol is one command per line. Every command gets exactly one final line, `ok ...` or `err <reason>`; data
lines come before it, and lines starting with `#` are comments (the boot banner). Times are microseconds on the robot's
own clock (`esp_timer`). Messages are hex MIDI bytes, and cables are 0 and 1 (USB jacks 1 and 2).

| Command | Reply | What it does |
|---|---|---|
| `id` | `ok id mac=.. fw=.. cables=2` | identity |
| `ping` | `ok pong` | |
| `time` | `ok time <us>` | the robot's clock, for tying clocks together |
| `at <t_us> <cable> <hex>` | `ok at` | schedule a message at `t_us` after `go`. Times may not go back (`err order`) |
| `at <t_us> unplug [ms]` / `at <t_us> plug` | `ok at` | a scheduled simulated unplug, back after `ms` |
| `rep <start_us> <interval_ns> <count> <cable> <hex>` | `ok rep <slot>` | a message on a fixed grid (a clock), 4 slots |
| `go [delay_us]` | `ok go t0=<us>` | start the schedule (default 10 ms from now) |
| `stop` / `clear` | `ok ...` | stop; clear also empties the schedule, logs and counters |
| `send <cable> <hex>` | `ok send` | send now |
| `unplug [ms]` / `plug` | `ok ...` | drop off the bus with `tud_disconnect`, and come back after `ms` (or on `plug`) |
| `status` | `ok status t=.. running=.. mounted=.. sent=.. recv=.. not_sent=.. tx_retries=.. unplugs=.. mounts=.. sched=.. sched_free=.. reps=.. log=.. log_lost_tx=.. log_lost_rx=..` | counters |
| `log [max]` | records, then `ok log n=<k> more=<left>` | drain up to `max` records (default 256), oldest first |

A log record is `<kind> <t_us> <packet>`, where the packet is the 4-byte USB-MIDI event packet in hex
(`cable << 4 | CIN`, then the MIDI bytes). The kinds are `s` sent, `r` received, `u` unplugged, `p` plugged back,
`M` mounted by the host, and `U` unmounted. A send is logged when TinyUSB takes it. If the TX FIFO is full, the robot
retries for 50 ms and then counts the message in `not_sent`. While the robot is unplugged it doesn't queue sends; it
counts them in `not_sent`. So the log holds exactly what left the robot. SysEx and running status aren't supported.

How the timing works:

- **Sends.** A protocol task on core 1 sleeps a tick while nothing is due for 2 ms. Closer to a due time it spins,
  yielding, so a send goes out within microseconds of its time.
- **Receives.** Received packets are stamped in `tud_midi_rx_cb`, which runs in TinyUSB's task at the top priority,
  so the stamps don't wait on the protocol task. There's one stamp per USB transfer, so the resolution is the host's
  polling.
- **Draining the log.** The log goes out a few lines at a time, and only while the UART's buffer has room, so a long
  drain never delays a send.

## Scenarios and checks

```sh
python3 -I tools/robot.py run tools/robot_scenarios/loopback.json --robot a=/dev/cu.usbserial-XXXX --mac a=aa:bb:..
python3 -I tools/robot.py run tools/robot_scenarios/merge.json --robot a=robot-2b28.local --robot b=robot-a23c.local \
    --mac a=44:bd:8d:61:2b:28 --mac b=a0:85:e3:f0:a2:3c
python3 -I tools/robot.py run tools/robot_scenarios/merge.json ... --duration 8h --progress build/robot/merge.progress.jsonl
python3 -I tools/robot.py run tools/robot_scenarios/merge.json --sim --sim-opt=--drop-every=50   # no hardware
build/tools-venv/bin/python tools/robot.py cmd --port /dev/cu.usbserial-XXXX status "log 10"         # by hand
```

A real UART port needs pyserial (`make`'s tools venv has it); Wi-Fi robots, `--sim` and the tests don't. Opening a
UART port holds DTR and RTS so the DevKitC's auto-reset circuit doesn't reset the robot. Each run appends one JSON line
to `build/robot/runs.jsonl`. `--save run.jsonl` streams the logs and clock ties to a file as they come, and
`robot.py check run.jsonl` checks them again (it also reads night 0's one-object saves). `--progress FILE` appends the
numbers so far every `--progress-s` (60 s), so a run that dies at hour 7 keeps its first 7 hours.

A scenario has three parts:

- **`streams`** say what each robot plays: notes, CC sweeps, a clock (a `rep`), raw messages, and unplugs.
- **`routes`** say what the P4 is meant to do with it: from a robot's cable, through an optional filter (channels, a
  note range, message types) and map (channel, transpose), to a robot's cable.
- **`listen`** names cables that take what the P4 makes itself, such as its clock.

The routes don't configure the P4: its own config has to match them. `duration_ms` sets a minimum length (for a
scenario that only listens). `repeat` plays the streams' pattern that many times, `period_ms` apart (default: one pass
plus `gap_ms`, 100); a clock runs once, from its start to the end of the last pass. `--repeat N` and `--duration 8h`
set it from the command line, so every scenario is also a soak. Stretching a scenario raises a listened-for clock's
`ticks_min` by the ticks the extra time holds, so a P4 clock that stops at minute 3 of 8 hours fails.

### One scenario per hub use case (C1-2)

The charter's use cases, each on the two Wi-Fi robots through the P4. Robot cables are 0 and 1 here; the P4's config
sees them as USB jacks 1 and 2. "A computer joins" isn't here: it needs the Mac, not a robot.

| Use case | Scenario | What runs | Pass/fail limits |
|---|---|---|---|
| A controller plays a synth | `controller-to-synth.json` | a plays 1,000 notes and 500 CCs on cable 0; b listens on cable 0 | loss 0, extra 0, order, latency 5 ms (past the clock basis), jitter 1 ms, stuck notes 0 |
| Two controllers, one synth | `merge.json` | a (keys, ch 1) and b (pads, ch 10) both play; b's cable 1 takes the merge | loss 0 and order per source, extra 0, latency 5 ms, jitter 1 ms, stuck 0 |
| One keyboard, two synths | `split.json` | a plays across the keyboard; below 60 to b cable 0 on ch 2, 60 and up to b cable 1 on ch 3 +12; CCs to both | loss 0, extra 0 (the wrong side of the split is extra), order, latency, jitter, stuck 0 |
| The box keeps time | `clock-out.json` | out: the P4's clock at 120 BPM to a and b cable 0; in: a's 120 BPM clock on cable 1 through the P4 to b cable 1 | clock jitter 1 ms (every interval), ticks at least 2,800 a minute, loss 0 and extra 0 on the clock through |
| Plugged and unplugged mid-song | `hotplug-song.json` | a plays to b; a unplugs every 2 s (300 ms, mid-note), b every 3 s (400 ms): 50 a pass | loss 0 outside the outages, extra 0, no stuck notes, note held at most 20 ms, every replug remounted within 2 s |
| (one robot, exact latency) | `loopback.json`, `hotplug.json` | a's cable 0 back to its own cable 1 | as above, on one clock |

The clock checks report the deviation of every interval from the nominal one: `clock_jitter_ms` (the worst),
`<robot><cable>_clock_dev_ms_p99` and `_clock_interval_ms_stdev`, so an hours-long run gives the jitter's shape, not
only its worst tick.

### The checks

All read from the logs:

| Check | Fails when |
|---|---|
| `accounted` | the logs don't hold every packet the robots counted, or a log overflowed (the run proves nothing) |
| `loss` | a routed message never arrived. Messages around an unplug count as `lost_unplugged` instead: from the unplug to the host's next mount, plus 100 ms |
| `extra` | an arrival no route explains (a duplicate, a stray, the wrong side of a split). A note-off or all-notes-off at a destination while one of its sources is unplugged is the P4 releasing that source's notes: `released`, not extra |
| `order` | arrivals out of send order, per route |
| `latency` / `jitter` | the worst latency, and the jitter (p99 − p50), over the limits. Across two robots, latency fails only when it's over the limit by more than `latency_basis_us`, and jitter is each latency against the median of its 64 neighbours on the route (`jitter_cross_ms`), so the clock tie's error drops out. Within one robot both are exact |
| `clock_jitter` | a received clock interval strays from the sent one, or from `listen`'s BPM, by more than the limit |
| `stuck_notes` | a receiver is left holding a note-on at the end. A receiver that unplugs lets go of what it held |
| `note_hold` | a receiver held a note longer than `note_hold_ms_max` (`note_hold_ms_max` is reported either way). It catches a note the P4 failed to release even when a later note-off for the same key ends it |
| `remounted` | a replug isn't followed by the host mounting the robot within `reenum_ms_max` (default 2 s). Without a mount, the unplug window closes there, so later losses count |

More numbers come with every run: `reconnect_ms`, from the end of an unplug to the host's mount; `not_sent`; and
`<robot>_syncs_rejected`. Mount events (`M`, `U`) come from the core's USB event task; the robot logs them at its next
poll, a little after their time, and the checker allows 200 ms for that.

Matching: the checker estimates the latency for each source and destination robot (the median of the last 4,096
arrivals, each against the nearest send of the same message), then matches each arrival to the expectation with the
same content nearest to (arrival − latency), within a window of max(5 ms, 8 × the median deviation). That way one
dropped or doubled clock tick is counted once, not as a shift of every tick after it. The limit is that identical
messages must be spaced further apart than the latency plus its jitter plus the clock tie's error. (Night 0 took the
latest send before each arrival; across two robots tied over Wi-Fi that flips to the previous repeat as soon as the
tie's error passes the latency, and on the sim it failed a clean run with 6,425 lost.)

Clocks: latency is exact within one robot (`loopback.json`: cable 0 out, through the P4, back into cable 1, on one
clock). Between robots, the clocks are tied to the Mac's with the fastest of 8 `time` requests at the start, every
`--sync-s` (10 s), and at the end. Each tie is off by up to half its round trip, which over Wi-Fi is milliseconds (5
pings on 2026-11-16: 3.7 to 53 ms to Big-S3, 4.3 to 106 ms to Small-V2). So at each tie the checker fits a line to the
last 30, weighted by 1 / round trip², and blends from one tie's line to the next, which also follows drift. A tie
with a round trip over 3 × the best of the last 6 (and 5 ms more) is skipped, never more than 3 in a row.
`latency_basis_us` is half the worst round trip used: on Wi-Fi the cross-robot latency check is loose, and the jitter
check, which doesn't depend on the tie, is the one that bites. For exact latency use a same-robot route. A jumper
between two robots' GPIOs carrying a sync edge would give sub-microsecond ties; that isn't built.

`--sim-tie-ms 4:50` gives the simulator's ties Wi-Fi-like round trips (a perfect tie is otherwise free in virtual
time). Measured night 1, 10 minutes each: with 4–50 ms ties every scenario passes (cross-robot jitter 0.108 to 0.11 ms
against 0.093 to 0.098 with perfect ties; latency max 5.97 ms, basis 7.5 ms), with 4–106 ms too (jitter 0.11 to 0.174,
latency max 9.3, basis 16.9), and 3 ms of P4 jitter still fails (jitter 1.602).

### Long runs

Nothing in a run grows with its length:

- **The schedule** is made as the run takes it: each stream is a generator, merged in time order (the same order, line
  for line, as night 0's `expand()`; the tests hold it to that on every scenario and 300 random ones). A billion notes
  cost nothing until they're sent.
- **The checker** (`Checker` in `robot.py`) takes records as each log drains. Once every robot's log is drained past a
  point and its clock is tied past it, it checks everything before that point in Mac-time order, and keeps only what a
  later record could still change: unmatched arrivals for the match window, expectations until no arrival can reach
  them, losses until the unplug windows around them are known. That's a few seconds of records. Latencies and clock
  intervals are kept as counts per microsecond, so the percentiles are exact. The checks and numbers are night 0's;
  the tests hold every made-up case to night 0's checker (`tests/robot_check_ref.py`), and a run checked as it drains
  to the same run checked whole.

On `build/robot_sim`, night 1, at the final code (virtual time, two robots, 200 µs of P4 jitter, Wi-Fi-like clock
ties `--sim-tie-ms 4:50`, `--duration 8h`, six at once on an M4 Pro):

| Scenario | Matched | Lost | Other | Wall time | Max RSS |
|---|---|---|---|---|---|
| `controller-to-synth` 10 s (for scale) | 2,500 | 0 | | 0.23 s | 33.3 MB |
| `controller-to-synth` 8 h | 7,132,500 | 0 | jitter 0.107 ms; latency p50 1.142 / max 5.974 ms (basis 11.2 ms) | 405.6 s | 45.8 MB |
| `merge` 8 h | 10,270,800 | 0 | jitter 0.108 ms | 758.4 s | 49.6 MB |
| `split` 8 h | 7,130,400 | 0 | jitter 0.104 ms | 387.7 s | 45.7 MB |
| `clock-out` 8 h | 1,385,278 through; 1,385,305 ticks heard by each listener | 0 | clock jitter on the clock through 0.18 ms p99 | 310.7 s | 37.8 MB |
| `hotplug-song` 8 h | 3,350,400 | 0 (1,185,600 lost_unplugged) | 14,400 + 9,600 unplugs, all remounted; note hold max 5.2 ms | 330.1 s | 41.0 MB |

All six passed. The checker held at most 7,633 to 28,277 records at once (`buffered_peak`), about 30 to 40 seconds'
worth: that's when three ties in a row are skipped (431 of about 2,880 were, at 4–50 ms), and it doesn't grow with
the run. With perfect ties (an earlier build, the same night) the peak was about one tie interval of records and the
RSS 32.6 to 34.8 MB. A short run has few ties to fit, so its cross-robot latency carries more of their error (the 10 s
run's p50 is 4.75 ms); its jitter doesn't.

## The simulator

`build/robot_sim` runs the robots' real core (`robot_core.cpp`) in virtual time behind a pretend P4, so a scenario
runs in seconds. `robot.py run --sim` starts one with a robot for each the scenario names and the scenario's routes as
the P4's (`sim_options()`), and the P4's clock for each `listen` with a clock. Options for faults and clocks go through
`--sim-opt`: `--latency-us`, `--jitter-us`, `--drop-every`, `--dup-every`, `--swap-every`, `--reenum-ms`,
`--p4-clock-jitter-us`, `--offset-us k:US` and `--drift-ppm k:PPM` (robot k's clock), `--no-release` (the P4 doesn't
release a departed source's notes). `robot.py run --sim-tie-ms LO:HI` adds Wi-Fi-like clock ties on the Mac side. The tests run every scenario clean and each fault against the check it should
fail.

## Not verified (night 1, on hardware)

1. The two-cable descriptor enumerates on the P4 and on the Mac, and both cables carry traffic. The layout was checked
   on paper: the same macros, compiled on the Mac, give 124 bytes, a MIDI Streaming header total of 97, jacks 1–4 for
   cable 1 and 5–8 for cable 2, and both endpoints listing their two embedded jacks.
2. `tud_disconnect()` / `tud_connect()` make the P4 see an unplug and a re-enumeration. The core's ARDUINO_USB events
   give `M` and `U`. Whether `U` comes after a software disconnect at all is unknown.
3. Receive stamp precision under load, and send accuracy: the core is tested, but the board isn't.
4. Opening the UART port doesn't reset the robot, and 921600 baud holds without loss through the bridge.
5. Long soaks on the robots themselves: the Mac side is ready (above), but a robot's 2,048-record logs must drain over
   Wi-Fi faster than they fill for hours, and the clock ties over Wi-Fi are untested.
6. Whether the P4 releases a departed controller's notes (`hotplug-song.json` assumes it should).

## Wi-Fi robots (Big-S3, Small-V2)

Big-S3 (`44:bd:8d:61:2b:28`, Waveshare 3.5") and Small-V2 (`a0:85:e3:f0:a2:3c`, Waveshare 1.69") each have one
USB port, the native one, and no USB-UART bridge. On the P4's hub that port is the robot's MIDI device, so the
`ROBOT_WIFI` build moves everything else to Wi-Fi:

- **Control:** the same line protocol on TCP port 7000 (`firmware/bench_robot/robot_net.cpp`). A task on core 0 owns
  the socket; the protocol task on core 1 only touches two stream buffers, so the network never delays a send. Every
  time is still stamped on the robot, so Wi-Fi only widens the time-sync basis between robots (milliseconds rather
  than tens of microseconds over a UART). Jitter, loss, order and one robot's latency are unaffected.
- **Name:** `robot-<last two MAC bytes>.local` (`robot-2b28.local`, `robot-a23c.local`). `id` adds
  `link=wifi ip=... rssi=... host=...`. Anywhere `robot.py` takes a port, it takes a host instead:
  `--robot a=robot-2b28.local`, `cmd --port robot-2b28.local id`.
- **Network:** the one Outrigger saved in NVS namespace `wifi` (`ssid`, `pass`), read-only. Flashing never writes
  NVS, so a board that ran Outrigger already knows the network.
- **Reflashing:** ArduinoOTA on port 3232. `make ota-bench-robot ROBOT_HOST=robot-2b28.local
  ROBOT_MAC=44:bd:8d:61:2b:28` checks the robot's `id` MAC, pushes the image, waits for it to come back and checks it
  runs the new build. No password: the lab network only.
- **Smaller logs:** 2,048 sent and 2,048 received records (the UART build has 4,096 each), since Wi-Fi needs about
  60 KB of internal RAM. `robot.py` drains during a run; a full log counts `log_lost_*` in `status`.

### The first flash (by cable, once per board)

With the board's port on the Mac and the board in Computer mode (or download mode: hold BOOT, tap RST):

```
make flash-bench-robot-wifi ROBOT_PORT=/dev/cu.usbmodemX ROBOT_MAC=44:bd:8d:61:2b:28
```

1. Checks the MAC, staying in download mode.
2. Backs up the whole flash to `build/robot-backups/<mac>-<time>.bin`, once per board. The first backup is the
   Outrigger image, the way back.
3. Refuses if that backup's NVS has no saved Wi-Fi network (the robot could never be reached).
4. Writes the bootloader (0x0), the partition table (0x8000), otadata (0xe000) and the app (0x10000), each at its own
   offset. Never the merged image at 0x0: its padding would erase NVS at 0x9000.

The robot's table (`firmware/bench_robot/partitions.csv`) keeps `nvs` and `otadata` where Outrigger has them and ends
at 0x3D0000, where Outrigger's store starts, so FM-1 backups and saves in the store survive. Prove an OTA while the
board is still on the Mac's cable, then move it to the P4's hub.

### Back to Outrigger

Write the backup back: `esptool --chip esp32s3 --port <port> write-flash 0x0 build/robot-backups/<mac>-<time>.bin`,
with the board in download mode (the robot holds the native port as a MIDI device, so the BOOT button is the way in).
