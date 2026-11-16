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
| `tests/robot_sim.cpp` | the same core behind a pretend P4, in virtual time (`robot.py run ... --sim`) |
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

Night-0 build: 374,470 bytes of flash (28%); 235,248 bytes of RAM in globals (71%), leaving 92,432. Most of that is
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
python3 -I tools/robot.py run tools/robot_scenarios/merge.json --robot a=... --robot b=...
python3 -I tools/robot.py run tools/robot_scenarios/loopback.json --sim --sim-opt=--drop-every=50   # no hardware
build/tools-venv/bin/python tools/robot.py cmd --port /dev/cu.usbserial-XXXX status "log 10"         # by hand
```

A real port needs pyserial (`make`'s tools venv has it); `--sim` and the tests don't. Opening the port holds DTR and
RTS so the DevKitC's auto-reset circuit doesn't reset the robot (to confirm on the board: night 1). Each run appends
one JSON line to `build/robot/runs.jsonl`; `--save run.json` keeps the logs, and `robot.py check run.json` checks them
again.

A scenario has three parts:

- **`streams`** say what each robot plays: notes, CC sweeps, a clock (a `rep`), raw messages, and unplugs.
- **`routes`** say what the P4 is meant to do with it: from a robot's cable, through an optional filter (channels, a
  note range, message types) and map (channel, transpose), to a robot's cable.
- **`listen`** names cables that take what the P4 makes itself, such as its clock.

The routes don't configure the P4: its own config has to match them.

The checks, all read from the logs:

| Check | Fails when |
|---|---|
| `accounted` | the logs don't hold every packet the robots counted, or a log overflowed (the run proves nothing) |
| `loss` | a routed message never arrived. Messages around an unplug count as `lost_unplugged` instead: from the unplug to the host's next mount, plus 100 ms |
| `extra` | an arrival no route explains (a duplicate, a stray, the wrong side of a split) |
| `order` | arrivals out of send order, per route |
| `latency` / `jitter` | the worst latency, and p99 − p50, over the limits |
| `clock_jitter` | a received clock interval strays from the sent one, or from `listen`'s BPM, by more than the limit |
| `stuck_notes` | a receiver is left holding a note-on |

Two more numbers come with every run: `reconnect_ms`, from the end of an unplug to the host's mount, and `not_sent`.

Matching: the checker first estimates the latency, then matches each arrival to the expectation with the same content
nearest to (arrival − latency), within a window of max(5 ms, 8 × the median deviation). That way one dropped or doubled
clock tick is counted once, not as a shift of every tick after it. The limit is that identical messages must be
spaced further apart than the latency plus its jitter.

Latency is exact within one robot (`loopback.json`: cable 0 out, through the P4, back into cable 1, on one clock).
Between robots, the clocks are tied through the UART, using the fastest of 8 `time` requests at the start and at the
end, which also corrects drift. The result carries the uncertainty as `latency_basis_us`, half the round trip. Jitter
doesn't depend on that offset. For tighter cross-robot latency, a jumper between two robots' GPIOs carrying a sync
edge would give sub-microsecond ties; that isn't built.

## Not verified (night 1, on hardware)

1. The two-cable descriptor enumerates on the P4 and on the Mac, and both cables carry traffic. The layout was checked
   on paper: the same macros, compiled on the Mac, give 124 bytes, a MIDI Streaming header total of 97, jacks 1–4 for
   cable 1 and 5–8 for cable 2, and both endpoints listing their two embedded jacks.
2. `tud_disconnect()` / `tud_connect()` make the P4 see an unplug and a re-enumeration. The core's ARDUINO_USB events
   give `M` and `U`. Whether `U` comes after a software disconnect at all is unknown.
3. Receive stamp precision under load, and send accuracy: the core is tested, but the board isn't.
4. Opening the UART port doesn't reset the robot, and 921600 baud holds without loss through the bridge.
5. Long soaks: `robot.py` keeps a whole run in memory and expands streams up front. That's fine for minutes. An
   8-hour run (C1-1) needs a windowed checker and lazy stream expansion.

## Big-S3 as a robot

Big-S3 (Waveshare ESP32-S3-Touch-LCD-3.5) has one USB port, the native one, and no USB-UART bridge. With that port on
the P4's hub:

- **Control moves to Wi-Fi.** The core is transport-agnostic, taking lines in and sending lines out, so a TCP socket
  carrying the same protocol is a small `robot_main` variant. Big-S3 is already on the lab Wi-Fi (`big-s3.local`).
  Wi-Fi timing doesn't matter for the checks, because every time is stamped on the robot. The cost: clock ties through
  `time` over Wi-Fi are coarser than over the UART (milliseconds rather than tens of microseconds), so cross-robot
  latency gets a wider `latency_basis_us`. Jitter, loss and order are unaffected, as is latency within one robot.
- **Alternatively, an external USB-UART adapter** on two free GPIOs keeps the UART protocol unchanged.
- **Reflashing needs recabling** (the one port, to the Mac) or OTA (`lib/ota`, as the lab boards already do).
- **No `CDCOnBoot`:** with USBMode=default the native port is entirely the MIDI device. A composite CDC-plus-MIDI
  device would put the serial port on the P4's side, out of the Mac's reach.
- **The unplug works the same way** (`tud_disconnect` on the same USB controller).
- **The screen could show the counters,** using the board drivers in `lib/board` and its pin map from
  `firmware/boards.json`. That's extra work, not needed for the checks.
- **Its RAM has room to spare:** 8 MB of octal PSRAM (`firmware/boards.json`), so the logs could be much larger.

So Big-S3 is a fine second robot once there's a Wi-Fi transport. A DevKitC needs no changes.
