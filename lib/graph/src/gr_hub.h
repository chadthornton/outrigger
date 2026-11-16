// The MIDI hub runtime (spike, night 1, track S2): track D's graph packaged
// for a firmware. Core 0 (USB host, Wi-Fi, diag page) feeds MIDI in and takes
// MIDI out; core 1 runs the graph. Everything between them goes through
// single-writer rings (CONTRACT.md, "Two flows").
//
//   Core 0, the USB host task, for every MIDI message from endpoint `in`:
//       gr_hub_in_midi1(&hub, in, now_us, bytes, len);
//   Core 1, the graph task, every block (any 1 .. GR_MAX_FRAMES frames):
//       gr_hub_run(&hub, gr_hub_frames_due(&hub, now_us), now_us);
//   Core 0, the USB host task, as often as it can:
//       while (gr_hub_out_midi1(&hub, out, now_us, bytes, &len)) send(out, bytes, len);
//   Core 0, the diag page, whenever:
//       gr_hub_stats s; if (gr_hub_stats_take(&hub, &s)) ...;
//
// Timing: an input stamped at t_us plays on the frame for t_us + latency_frames
// (room for core 1 to be mid-block when it arrives), and leaves its output
// endpoint at that frame's time + out_delay_us (room for core 1 to wake late:
// outputs are released by gr_hub_out_* only once due). So the hub adds one
// constant latency, latency_frames / sample_rate + out_delay_us, and neither
// block sizes nor core 1's wake-up jitter reach the wire as long as core 1
// wakes within out_delay_us. Time is the caller's
// microsecond clock (esp_timer_get_time() truncated to 32 bits; wrap-safe).
//
// Endpoints are numbers: HW2 maps (USB device, cable) to an index. Each
// in-ring has one producer and each out-ring one consumer: call every
// gr_hub_in_* for one endpoint from one task, and every gr_hub_out_* for one
// endpoint from one task.
#ifndef GR_HUB_H
#define GR_HUB_H

#include <stdint.h>

#include "gr_graph.h"
#include "gr_nodes.h"
#include "gr_spsc.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef GR_HUB_ENDPOINTS
#define GR_HUB_ENDPOINTS 4  // in endpoints, and out endpoints
#endif
#ifndef GR_HUB_RING
#define GR_HUB_RING 256  // events per endpoint ring (power of two)
#endif
#define GR_HUB_STATS_NODES 16

// What crosses cores at the hub's edges: an event with a microsecond time.
typedef struct gr_tevent {
  uint32_t t_us;  // in: when it arrived; out: when it is due on the wire
  uint32_t ump[4];
} gr_tevent;

// The block clock's tie to the caller's microsecond clock (core 1 only).
typedef struct gr_hub_clock {
  uint32_t anchor_us;     // microsecond time of anchor_frame
  uint64_t anchor_frame;  // an absolute frame
  uint32_t sample_rate;
  uint32_t latency_frames;
  uint32_t out_delay_us;
  uint8_t started;
  uint32_t resyncs;       // times the clock was re-anchored (a stall, or a jump)
} gr_hub_clock;

// Edge nodes. tsource: 0 in, 1 event out, pops its ring's events whose time
// (plus latency) falls before the block's end. tsink: 1 event in, 0 out,
// pushes each event with its due time.
typedef struct gr_tsource {
  gr_spsc *q;
  gr_hub_clock *clk;
  uint32_t late;   // landed on frame 0 because their frame had passed
  uint32_t taken;
} gr_tsource;
typedef struct gr_tsink {
  gr_spsc *q;
  gr_hub_clock *clk;
  uint32_t sent;
} gr_tsink;
extern const gr_node_desc gr_tsource_desc;
extern const gr_node_desc gr_tsink_desc;

// The C1-2 use cases, as hub apps. in/out are endpoint numbers.
typedef enum {
  GR_HUB_APP_THRU = 0,  // in 0 -> out 0 (a controller plays a synth)
  GR_HUB_APP_MERGE,     // in 0 + in 1 -> out 0 (two controllers, one synth)
  GR_HUB_APP_SPLIT,     // in 0: below `split_key` -> out 0 on `ch_lower`; the rest -> out 1 on `ch_upper`
  GR_HUB_APP_LAYER,     // in 0 -> out 0, and transposed by `transpose` -> out 1 on `ch_upper`
  GR_HUB_APP_CLOCK,     // in 0 -> out 0, plus the hub's MIDI clock at `bpm` merged into out 0 and out 1
  GR_HUB_APP_COUNT
} gr_hub_app;

typedef struct gr_hub_config {
  gr_hub_app app;
  uint32_t sample_rate;     // 48000
  uint32_t default_frames;  // 48 (1 ms at 48 kHz)
  uint32_t latency_frames;  // input side; 0 = one default block (1 ms)
  uint32_t out_delay_us;    // output side; 0 = two default blocks (2 ms)
  uint32_t cpu_mhz;         // ticks per us for per-node time; 0 on the host (ns)
  uint8_t split_key, ch_lower, ch_upper;
  int8_t transpose;
  float bpm;
} gr_hub_config;
void gr_hub_config_default(gr_hub_config *c, gr_hub_app app);

// The apps' routing nodes (only some are used by any one app).
typedef struct gr_hub_app_nodes {
  gr_merge merge[3];
  gr_split split;
  gr_chanmap chanmap[2];
  gr_transpose transpose;
  gr_clockgen clockgen;
} gr_hub_app_nodes;

// An app's routing, between its inputs and its outputs, on any graph: the hub
// puts tsource/tsink nodes around it; the checker (tools/graph_check.c) puts
// its own feeds around it. in[i] is the node feeding input i (event port 0).
// Fills out_node/out_port for each output endpoint; returns their number, or < 0.
int gr_hub_app_inputs(gr_hub_app app);
int gr_hub_app_build(gr_graph *g, const gr_hub_config *c, gr_hub_app_nodes *m, const int *in, int *out_node,
                     int *out_port);

// A snapshot core 1 publishes for core 0 (32-bit fields only, so nothing
// tears; it crosses in a ring of snapshots, never as shared memory).
typedef struct gr_hub_node_stats {
  const char *name;   // the node's descriptor name (constant data)
  uint32_t last_ticks, max_ticks, mean_ticks;
} gr_hub_node_stats;
typedef struct gr_hub_stats {
  uint32_t blocks;       // low 32 bits
  uint32_t frames;       // low 32 bits of the block clock
  uint32_t ticks_per_us;
  uint32_t block_max_ticks;  // the whole graph's slowest block since init
  uint32_t ev_dropped;   // event-list overflows inside the graph
  uint32_t in_dropped;   // core 0 pushes refused (in-ring full), all endpoints
  uint32_t out_dropped;  // core 1 pushes refused (out-ring full), all endpoints
  uint32_t late;         // inputs that arrived after their frame
  uint32_t in_events, out_events;
  uint32_t resyncs;
  uint8_t n_nodes;
  gr_hub_node_stats node[GR_HUB_STATS_NODES];
} gr_hub_stats;

typedef struct gr_hub {
  gr_graph g;  // ~78 KB: keep the hub static, never on a task's stack
  gr_hub_config cfg;
  gr_hub_clock clk;
  gr_spsc in_q[GR_HUB_ENDPOINTS], out_q[GR_HUB_ENDPOINTS];
  gr_tevent in_buf[GR_HUB_ENDPOINTS][GR_HUB_RING];
  gr_tevent out_buf[GR_HUB_ENDPOINTS][GR_HUB_RING];
  gr_tsource src[GR_HUB_ENDPOINTS];
  gr_tsink snk[GR_HUB_ENDPOINTS];
  int src_id[GR_HUB_ENDPOINTS], snk_id[GR_HUB_ENDPOINTS];
  uint8_t n_in, n_out;
  gr_hub_app_nodes app;  // the app's routing nodes
  uint32_t block_max_ticks;
  uint32_t in_on[GR_HUB_ENDPOINTS][16][4];  // core 0 only: notes each in-endpoint holds (for unplug)
  uint32_t stats_every;  // blocks between snapshots
  uint32_t stats_count;
  gr_spsc stats_q;
  gr_hub_stats stats_buf[4];
} gr_hub;

// Core 0, before core 1 starts. GR_OK or an error (gr_graph.h).
int gr_hub_init(gr_hub *h, const gr_hub_config *cfg);

// Core 0 (one producer per endpoint). 1 if queued; 0 if the ring was full
// (counted in in_dropped) or, for midi1, the message has no single-UMP form
// (SysEx; not counted).
int gr_hub_in_ump(gr_hub *h, int in, uint32_t t_us, const uint32_t *ump);
int gr_hub_in_midi1(gr_hub *h, int in, uint32_t t_us, const uint8_t *msg, int len);

// Core 0, when in-endpoint `in` is unplugged (or its device resets): pushes a
// note off for every note it still holds, so nothing sticks downstream (the
// routing nodes send each off where its note on went). Returns the offs
// queued. If the ring fills, the rest stay held: call it again later (never wait).
int gr_hub_in_release(gr_hub *h, int in, uint32_t t_us);

// Core 1. The frames the block clock is behind the microsecond clock: run a
// block of that many and the block clock follows real time exactly. 0 when
// nothing is due; at most GR_MAX_FRAMES (call again to catch up). Before the
// first block: default_frames.
uint32_t gr_hub_frames_due(gr_hub *h, uint32_t now_us);
// Core 1: one block of `frames` (0 runs nothing). now_us anchors the clock
// on the first block.
int gr_hub_run(gr_hub *h, uint32_t frames, uint32_t now_us);

// Core 0 (one consumer per endpoint). The oldest event on `out` that is due
// at now_us; 1 if one was taken. midi1 skips (and drops) UMPs with no MIDI
// 1.0 form; len gets 1..3.
int gr_hub_out_ump(gr_hub *h, int out, uint32_t now_us, gr_tevent *ev);
int gr_hub_out_midi1(gr_hub *h, int out, uint32_t now_us, uint8_t msg[3], int *len);

// Core 0: the newest snapshot (published every ~100 ms of audio). 1 if one was taken.
int gr_hub_stats_take(gr_hub *h, gr_hub_stats *out);

// Wrap-safe: is a at or after b on the 32-bit microsecond clock?
static inline int gr_us_reached(uint32_t a, uint32_t b) { return (int32_t)(a - b) >= 0; }

#ifdef __cplusplus
}
#endif

#endif
