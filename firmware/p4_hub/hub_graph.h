// The graph on Big-P4 (HW2, night 1): track D's gr_hub (lib/graph) between the
// USB host's MIDI in and out. GET /graph shows it; POST /bench/graph sets it.
//
// Threads (lib/graph/CONTRACT.md, "Two flows"):
// - core 1, task "graph": gr_hub_run() each FreeRTOS tick; only 32-bit counters of its own.
// - core 0, EspUsbHost's client task: every gr_hub_in_* (MIDI in, and the
//   note-offs for an unplugged source), so each in-ring has one producer.
// - core 0, task "graph_out": every gr_hub_out_*, woken by an esp_timer at the
//   head event's due time, so each out-ring has one consumer.
// - the web task: the only gr_hub_stats_take() caller (graph_state_json).
#pragma once
#include <stddef.h>

void graph_begin();
size_t graph_state_json(char *out, size_t cap);
// POST /bench/graph?QUERY: app=thru|merge|split|layer|clock, inN=a0 (robot a, cable 0),
// outN=b1[,a0] (fan-out to two), ch_lower=, ch_upper=, transpose=, split_t=,
// split_key=, bpm=, plain=1 (clock: out 0 without the clock), reset=1 (counters
// only). Stops core 1, re-inits the hub, starts again. Writes a JSON reply.
size_t graph_configure(const char *query, char *out, size_t cap);
