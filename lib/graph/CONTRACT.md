# The node contract

What a node, an app or a firmware has to do to run in lib/graph. Each rule comes with what checks it: a test, a tool, or "review only". These rules are taken from the VIBE FM SDK's lessons (README, "Two flows"; `tools/vibe-check.mjs`) and applied to our two cores. VIBE's code isn't used here, only its rules.

## 1. Blocks: take the host's size

- A block is 1 to `GR_MAX_FRAMES` (256) frames at `sample_rate`, and its size can change from one block to the next. The host decides. USB audio at 48 kHz gives 48 frames per 1 ms USB frame. A FreeRTOS tick gives 48, and Schwung uses 128.
- A node reads `b->frames` and `b->sample_rate` on every call and assumes nothing about either. It never assumes 64.
- **Frames:** every event a node writes sits on a frame from 0 to `b->frames - 1`, and each event list stays in frame order. A node writes `b->frames` audio samples per output, never more.
- **State:** a node's state lives in absolute frames (`b->frame_pos + frame`), never in a block count. Fed the same events at the same absolute frames, a node gives the same output whatever the block sizes. This is called partition invariance.
  - A node that can't keep partition invariance declares it. Schwung's own block-quantised tick, `tick_quantum > 1`, is one such case.
- **Event capacity:** each port holds `GR_MAX_EVENTS` (64) events per block. That is 64 per 1.3 ms at 64 frames, but only 64 per 5.3 ms at 256 frames. Overflow is counted in `dropped`, never silent.

Checked by:
- `test_graph`'s `test_block_partitions`: the hub graph gives the same output at 64, 48, 1, 7, 128 and 256 frames and at random mixes of sizes. Its positive control is a source that hardcodes 64.
- `build/graph_check` (section 4): it runs every node at block sizes 1 to 256.

## 2. Two flows

| Flow | Where | What runs there |
|---|---|---|
| **Audio** | core 1, the graph task | `gr_graph_run` / `gr_hub_run`: every node's `process`, nothing else |
| **Control** | core 0 | USB host, Wi-Fi, the diag page, the screen, storage, building graphs, changing parameters |

Data both flows touch is shared in one of three ways only:

1. **A value of 32 bits or less with one writer.** Use `__atomic_store_n` / `__atomic_load_n`, relaxed. A request is sent as a counter the writer increments; the other side remembers the last request it handled. 64-bit values tear on the P4 (RV32), so never share `frame_pos` or a timing total. Instead, publish a 32-bit snapshot (`gr_hub_stats`).
2. **Anything bigger goes through a ring with one writer and one reader** (`gr_spsc`). The writer writes the data, then publishes the counter with RELEASE. The reader reads the counter with ACQUIRE, then the data.
3. **Big state (a routing table, a scene) is handed over as a swap with acknowledge** (`gr_swap`). Control fills the spare copy, releases, and bumps `published`. Audio takes it at a block start and acknowledges with `taken`. Control writes only the copy audio no longer reads.

And:

4. **The audio flow never allocates, frees, logs, prints, locks, waits, sleeps or does file I/O.** A node's `init` (control flow) may allocate. Schwung modules `calloc` in `create_instance`, which `gr_schwung_init` calls. Its `process` may not.
5. **The control flow never waits for the audio flow.**
   - A full ring refuses the push at once and counts it: `gr_spsc_push` returns 0 and adds to `dropped`.
   - `gr_swap_back` returns NULL while the last swap is still pending.
   - Never spin on either.
6. **Parameters change between blocks, from the audio flow's side.** A node's fields are the audio flow's. Control sends a change as a rule-1 value, a rule-2 message or a rule-3 swap, and never writes a running node's struct directly.
   - The exception is a graph that isn't running yet: build it, then start core 1.
   - Notes remember their routing from note on, so a change mid-note never leaves a note stuck.

Checked by:

| Rule | Check |
|---|---|
| 1, the 64-bit rule | review. `gr_hub_stats` has 32-bit fields only. `gr_spsc_dropped()` reads a ring's drop count atomically: `graph-tsan` found the plain `dropped++` racing with the stats reader on 2026-11-16, and it was fixed. |
| 2 | `test_graph_flows`: 500,000 messages, 0 torn. `make graph-tsan` runs it and the hub's three-thread test under ThreadSanitizer. Its positive control weakens the ring's handover to relaxed, and the M4's own reordering then tears messages (4 of 5 runs tried) and fails the test. Also `GR_CHECK_FLOWS` builds: each ring records its first producer and its first consumer, and counts any other caller (`producer_violations`, `consumer_violations`). `test_graph_flows` proves a second writer is caught. |
| 3 | `test_graph_flows`' swap test: 20,000 tables, 0 torn, never backwards. |
| 4, static | `tools/graph_rt_symbols.sh`, run by `make test-graph` over the core, the hub and the shim. It fails any object that references an allocator, stdio, a lock, a wait, a sleep, file I/O, FreeRTOS waits or `esp_log`. Its positive control is `tests/support/gr_rt_bad.c`, which must be caught. |
| 4, runtime | `build/graph_check` counts allocations during `process` (section 4). |
| 5 | `test_graph_flows` and `test_graph_hub`: full rings refuse and count. Review: no wait loop on core 0 in `gr_hub`. |
| 6 | review. `build/graph_check`'s stuck-note run changes parameters between blocks and checks every note gets its off. |

## 3. A node

- A node is a caller-owned struct plus a `const gr_node_desc`: its name (1 to 31 characters), its typed ports (events or audio, up to `GR_MAX_PORTS` each way) and `process(self, block, io)`.
- Outputs are cleared or zeroed before each call. Unconnected inputs read as empty or as silence.
- A node never writes to its inputs. They are `const`, and the checker checksums them.
- No NaN or infinity in audio out.

## 4. The checker

`build/graph_check` checks a node or an app before it runs, the way `vibe-check` checks a Vibe. It prints one line per check: `PASS`, `FAIL` or `----` (not tested). It exits 0 when everything passes, 1 on any failure, and 2 when it couldn't run. `make test` runs it over every built-in node, both Schwung modules and the hub apps, and runs a set of deliberately broken nodes that must each fail their own check. See `tools/graph_check.c` for the list of checks.
