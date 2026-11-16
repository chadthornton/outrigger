# lib/graph: the real-time graph core (spike, night 0)

Milestones C1-1 (graph core) and C1-4 (Schwung shim) of the P4 platform
spike, host only. Plain C11, no allocation, builds for the Mac, the ESP32-P4
and the ESP32-S3 (`make graph-esp`).

| File | What |
|---|---|
| `src/gr_ump.h/.c` | MIDI 2.0 UMP fields, builders, MIDI 1.0 <-> 2.0 conversion (spec min-center-max scaling) |
| `src/gr_graph.h/.c` | Block clock, event lists, the node contract, typed ports, the graph, per-node timing |
| `src/gr_spsc.h/.c` | Lock-free single-producer single-consumer ring for core 0 <-> core 1 |
| `src/gr_nodes.h/.c` | source / sink (the rings at the graph's edges), merge, split, channel map, transpose, CC remap, impulse, pass |
| `src/gr_hub.h/.c` | The hub runtime for a firmware: per-endpoint rings, time-stamped edges, the C1-2 apps, stats (night 1; `docs/graph-p4.md`) |
| `src/gr_swap.h/.c` | Swap with acknowledge: big state from the control flow to the audio flow (night 1) |
| `check/` | The app checker's harness and the built-in subjects (`build/graph_check`, host only) |
| `CONTRACT.md` | The node contract: block sizes, the two flows, what checks each rule |
| `shim/gr_schwung.h/.c` | Schwung's `midi_fx_api_v1` on the node contract |
| `third_party/` | Schwung's API headers, eucalypso, superarp (all MIT; see its README) |

## The contract

- One block is 1 to `GR_MAX_FRAMES` frames at `sample_rate`, and its size can
  change every block: `gr_graph_run_frames(g, n)`. `gr_graph_run()` runs the
  default size given at init. The full contract: `CONTRACT.md`.
- An event is a UMP (1 to 4 words) plus a frame inside the block. Event lists
  stay in frame order.
- A node is a caller-owned struct plus a const `gr_node_desc`: its name, its
  typed input and output ports (events or audio), and
  `process(self, block, io)`. Outputs are cleared (events) or zeroed (audio)
  before each call; unconnected inputs read as empty or silence.
- `connect()` checks port types and allows one source per input (fan-out is
  free; fan-in is the merge node). `compile()` binds every output to a
  buffer from the graph's fixed pools and orders the nodes (cycles refused).
- Every node is timed every block: last, max, total, in `gr_clock_ticks()`
  units (ns on the host, CPU cycles on an ESP32: set `ticks_per_us`).
- Notes remember their routing at note on (split, channel map, transpose),
  so a change mid-note never leaves a note stuck.

## Commands

- `make test-graph`: the host tests, plus an `nm` check that the core and
  shim reference no allocator. `make test` runs the two test binaries
  (not the `nm` check).
- `make graph-bench`: per-node time per block on the host.
- `make graph-esp`: cross-compiles for P4 and S3 and prints sizes.
- `make graph-check`: the app checker over every built-in node and hub app.
- `make graph-tsan`: the threaded tests under ThreadSanitizer.
- `make graph-p4`: builds `firmware/graph_hub_p4` for Big-P4 (compile only).
- `./build/test_graph_schwung tests/golden/graph --record`: rewrites the
  shim's recordings (only after checking why they changed).

## Not done here

Night 0 is host only: no 8-hour robot run, no jitter on hardware, no P4
timing. The graph is single-threaded on its core; parameter changes from
another core would go through an SPSC ring too.
