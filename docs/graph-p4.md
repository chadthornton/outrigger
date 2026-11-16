# The graph on Big-P4: the interface for HW2

Spike, night 1, track S2. This is what HW2 needs to run track D's graph inside HW1's firmware: Big-P4 routing robot traffic from the hub's USB host (milestones C1-1 and C1-2).

## In short

1. Merge `spike/platform-p4-d-graph` into `spike/platform-p4-hw`. `git merge-tree` reported no conflicts on 2026-11-16. Only `lib/graph/`, `tests/`, `tools/graph_*`, `firmware/graph_hub_p4/`, this doc and a few lines in the Makefile's graph section change.
2. Build with `--library lib/graph`. The Arduino library format compiles `lib/graph/src/` only, which is the core, the routing nodes and the hub. The Schwung shim (`lib/graph/shim/`) is left out, which is fine for hub routing.
3. Copy the three call sites from `firmware/graph_hub_p4/hub_p4.cpp` (below) into the USB host code.
4. Start with `firmware/graph_hub_p4` on its own (`make graph-p4`, compile only): a synthetic load and one `STATS` line a second. Flash it the MAC-checked way, as `flash-synth-bench-p4` does on the hw branch.

## The calls

```c
#include "gr_hub.h"
static gr_hub hub;                       // ~129 KB. Static, never on a stack.

// setup(), before either task starts:
gr_hub_config c;
gr_hub_config_default(&c, GR_HUB_APP_MERGE);   // or THRU, SPLIT, LAYER, CLOCK
c.cpu_mhz = getCpuFrequencyMhz();              // REQUIRED on ESP: per-node time is in CPU cycles
gr_hub_init(&hub, &c);                          // GR_OK (0) or < 0

// Core 1, the graph task (pinned to core 1, priority above loopTask):
for (;;) {
  uint32_t t = (uint32_t)esp_timer_get_time(), f;
  while ((f = gr_hub_frames_due(&hub, t)) != 0) gr_hub_run(&hub, f, t);
  vTaskDelay(1);                                // 1 ms tick: 48 frames a block at 48 kHz
}

// Core 0, the USB host task, for each MIDI message from endpoint `in`:
gr_hub_in_midi1(&hub, in, (uint32_t)esp_timer_get_time(), bytes, len);   // len 1..3; 0 = dropped (counted)

// Core 0, the USB host task, as often as it runs:
uint8_t m[3]; int len;
while (gr_hub_out_midi1(&hub, out, (uint32_t)esp_timer_get_time(), m, &len)) send_to(out, m, len);

// Core 0, the diag page (E1), whenever it likes:
gr_hub_stats s;
if (gr_hub_stats_take(&hub, &s)) { /* s.node[i].name, .mean_ticks / s.ticks_per_us = us ... */ }
```

## Endpoints

An endpoint is a number, from 0 up to `GR_HUB_ENDPOINTS` (4). HW2 maps each (USB device, cable) pair to an endpoint. A robot is a two-cable device, so one robot can be two controllers. What each app reads and writes:

| App | Inputs | Outputs | C1-2 use case |
|---|---|---|---|
| `GR_HUB_APP_THRU` | in 0 | out 0 | a controller plays a synth |
| `GR_HUB_APP_MERGE` | in 0, in 1 | out 0 | two controllers, one synth (merged in time order) |
| `GR_HUB_APP_SPLIT` | in 0 | out 0: notes below `split_key` (60) on `ch_lower` (0); out 1: the rest on `ch_upper` (1); everything that isn't a note goes to both | one keyboard, two synths |
| `GR_HUB_APP_LAYER` | in 0 | out 0: as played; out 1: transposed by `transpose` (+12) on `ch_upper` | layered |
| `GR_HUB_APP_CLOCK` | in 0 | out 0: in 0 plus the clock; out 1: the clock alone (Start, then 24 ppqn at `bpm`) | the box keeps time |

Notes remember their routing from note on to note off, so a note never sticks when a split or transpose changes mid-note.

## Rules HW2 must keep (CONTRACT.md, "Two flows")

- **One producer per in-endpoint and one consumer per out-endpoint.** EspUsbHost delivers every device's data from the task that runs `usb_host_client_handle_events`. If one task does all the `gr_hub_in_*` and `gr_hub_out_*` calls, the rule holds. If callbacks for different devices ever run in different tasks, give each endpoint to exactly one of those tasks.
- **Core 0 never waits for core 1.** A full in-ring refuses the push. That returns 0 and counts in `stats.in_dropped`. Don't retry in a loop.
- **Core 0 never reads `hub.g` directly.** Its 64-bit counters (`frame_pos`, timing totals) tear on RV32. Read the snapshot from `gr_hub_stats_take`: 32-bit fields only, handed over through a ring about every 100 ms of audio.
- **Nothing on core 1 logs, allocates, locks or blocks.** The graph task's only wait is `vTaskDelay(1)`, between blocks.
- **Change apps or parameters only by re-initialising, and only while core 1 is stopped.** Live parameter changes from core 0 would go through a ring. That isn't built tonight.

## Timing

- An input stamped `t_us` plays on the frame for `t_us + latency_frames`, which defaults to one block: 48 frames, 1 ms. This leaves room for core 1 to be mid-block when it arrives.
- Its output is held in the out-ring until `that frame's time + out_delay_us`, which defaults to two blocks, 2 ms. This leaves room for core 1 to wake late.
- So the hub adds one constant latency: 3 ms by default. Block sizes and core 1's wake-up jitter never reach the wire, as long as core 1 wakes within `out_delay_us`. On the Mac simulation, 5,000 messages through `THRU` all took 3,000 us, and that held across the 32-bit microsecond wrap.
- If an input arrives after its frame, it plays on frame 0 of the next block and counts in `stats.late`. If core 1 stalls for more than 100 ms, the clock re-anchors once (`stats.resyncs`), and the stalled inputs count as late. Nothing is dropped.
- The clock app's clocks sit on exact frames, with the interval kept to 1/2^32 frame. Over 60 s on the Mac simulation, with ±400 us block jitter: 2,880 clocks, worst 33.3 us off the 120 bpm grid. That is one frame plus the simulation's 50 us poll.
- `gr_clockgen` keeps absolute frames in 32.32 fixed point. That is good for 2^32 frames, 24.8 hours at 48 kHz. Fine for an 8-hour soak.

## What HW2 should measure (none of it is measured yet: no hardware tonight)

- **Per-node time:** `stats.node[i].mean_ticks`, `.max_ticks` and `.last_ticks`, divided by `ticks_per_us` to get us. `block_max_ticks` covers the whole graph.
- **Latency on the wire, robot to robot:** it should be 3 ms plus the USB transfer time. The sketch's stand-in also prints `lat_us=min/mean/max` for its own loop.
- **The counters:** `in_dropped`, `out_dropped`, `ev_dropped` and `late`. All four should stay 0 through the soak.
- **Event density:** 64 events per port per block. At 48-frame blocks that is 64,000 events a second per port, far above MIDI's rate. Overflow is counted, never silent.
- **The 64-bit divides:** `tsource` and `tsink` call `__divdi3` per event (microseconds to frames). This is cheap at MIDI rates, but measure it. The fix, if needed, is a 32.32 multiplier.

## Sizes (measured 2026-11-16)

- `make graph-esp`, ESP32-P4, -Os, text bytes: gr_graph 1,686, gr_hub 3,618, gr_nodes 2,961, gr_spsc 336, gr_ump 986. Core plus hub: 9,587 B. No data or bss of their own.
- `make graph-p4` (`firmware/graph_hub_p4`, Arduino core 3.3.12): 334,814 B flash (10%). Global RAM is 150,648 B (45% of 327,680), most of it `gr_hub`: 128,960 B, with 4 endpoints × 2 rings × 256 events × 20 B. Two ways to shrink it if HW1's firmware is tight: `-DGR_HUB_RING=64`, or put the hub in PSRAM with `EXT_RAM_BSS_ATTR` (untested).
- Arduino's `loop()` runs on core 1 on the ESP32. The graph task sits above it in priority, so `loop()` gets core 1 only between blocks. Keep USB on core 0.

## Not supported tonight

- SysEx: `gr_hub_in_midi1` refuses it and returns 0.
- Live parameter changes.
- More than 4 endpoints.
- Audio nodes in the hub: the graph supports them, the hub apps don't use them.
