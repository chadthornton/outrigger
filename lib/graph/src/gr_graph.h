// The real-time graph core (spike, night 0, milestone C1-1).
//
//  - A block clock that takes the host's block size: every run is one block
//    of 1 .. GR_MAX_FRAMES frames at `sample_rate`, and the size may change
//    from one block to the next (gr_graph_run_frames). gr_graph_run() runs a
//    block of the default size given at init. No node may assume 64 frames:
//    see lib/graph/CONTRACT.md.
//  - Events are MIDI 2.0 UMPs, each stamped with a frame inside the block.
//    An event list is kept in frame order.
//  - One node contract: process(self, block, io), reading io->ev_in /
//    io->audio_in and writing io->ev_out / io->audio_out.
//  - Nodes declare typed ports (events or audio); connect() checks types.
//  - Every node is timed every block (last, max, total).
//  - Nothing allocates. The graph owns fixed pools sized by the macros below;
//    nodes are caller-owned structs. gr_graph_run() is the whole process path.
#ifndef GR_GRAPH_H
#define GR_GRAPH_H

#include <stdint.h>

#include "gr_ump.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef GR_MAX_FRAMES
#define GR_MAX_FRAMES 256  // largest block a graph accepts
#endif
#ifndef GR_MAX_EVENTS
#define GR_MAX_EVENTS 64  // events per port per block
#endif
#ifndef GR_MAX_PORTS
#define GR_MAX_PORTS 4  // inputs or outputs per node
#endif
#ifndef GR_MAX_NODES
#define GR_MAX_NODES 32
#endif
#ifndef GR_EVENT_POOL
#define GR_EVENT_POOL 48  // event output ports across the graph
#endif
#ifndef GR_AUDIO_POOL
#define GR_AUDIO_POOL 8  // audio output ports across the graph
#endif

#define GR_DEFAULT_SAMPLE_RATE 48000
#define GR_DEFAULT_FRAMES 64

typedef struct gr_event {
  uint32_t frame;  // 0 .. block frames - 1
  uint32_t ump[4];
} gr_event;

typedef struct gr_events {
  uint16_t count;
  uint16_t dropped;  // pushes refused this block because the list was full
  gr_event ev[GR_MAX_EVENTS];
} gr_events;

// Adds an event in frame order (stable: after any event at the same frame).
// Returns 1, or 0 when full (and counts a drop).
int gr_events_push(gr_events *e, uint32_t frame, const uint32_t *ump);
static inline void gr_events_clear(gr_events *e) {
  e->count = 0;
  e->dropped = 0;
}

typedef struct gr_block {
  uint32_t frames;     // this block's size: 1 .. GR_MAX_FRAMES, may differ every block
  uint32_t sample_rate;
  uint64_t index;      // blocks run so far
  uint64_t frame_pos;  // absolute frame of this block's frame 0
} gr_block;

typedef struct gr_io {
  const gr_events *ev_in[GR_MAX_PORTS];  // never NULL for an event port: empty if unconnected
  gr_events *ev_out[GR_MAX_PORTS];       // cleared before process
  const float *audio_in[GR_MAX_PORTS];   // silence if unconnected
  float *audio_out[GR_MAX_PORTS];        // zeroed before process; `frames` samples
} gr_io;

typedef enum { GR_PORT_NONE = 0, GR_PORT_EVENTS = 1, GR_PORT_AUDIO = 2 } gr_port_type;

typedef struct gr_node_desc {
  const char *name;
  uint8_t n_in, n_out;
  uint8_t in_type[GR_MAX_PORTS];
  uint8_t out_type[GR_MAX_PORTS];
  void (*process)(void *self, const gr_block *b, gr_io *io);
} gr_node_desc;

typedef struct gr_timing {
  uint32_t last;   // ticks spent in process, last block
  uint32_t max;
  uint64_t total;
  uint64_t blocks;
} gr_timing;

typedef struct gr_slot {
  const gr_node_desc *desc;
  void *self;
  int8_t src_node[GR_MAX_PORTS];  // per input: the node feeding it, or -1
  uint8_t src_port[GR_MAX_PORTS];
  gr_io io;
  gr_timing t;
} gr_slot;

typedef struct gr_graph {
  gr_block block;
  uint32_t default_frames;  // gr_graph_run()'s block size
  uint32_t ticks_per_us;  // timing clock rate (host: ns, so 1000)
  uint8_t n_nodes, compiled, n_ev, n_audio;
  uint8_t order[GR_MAX_NODES];
  gr_slot slot[GR_MAX_NODES];
  gr_events empty;
  float silence[GR_MAX_FRAMES];
  gr_events ev_pool[GR_EVENT_POOL];
  float audio_pool[GR_AUDIO_POOL][GR_MAX_FRAMES];
  uint32_t ev_dropped_total;  // sum of every output's drops since init
} gr_graph;

enum {
  GR_OK = 0,
  GR_ERR_ARG = -1,
  GR_ERR_FULL = -2,     // too many nodes, or a pool ran out
  GR_ERR_TYPE = -3,     // port types differ
  GR_ERR_TAKEN = -4,    // input already connected
  GR_ERR_CYCLE = -5,
  GR_ERR_COMPILED = -6, // edits after compile
};

int gr_graph_init(gr_graph *g, uint32_t sample_rate, uint32_t frames);
int gr_graph_add(gr_graph *g, const gr_node_desc *desc, void *self);  // node id, or < 0
int gr_graph_connect(gr_graph *g, int src, int src_port, int dst, int dst_port);
int gr_graph_compile(gr_graph *g);  // binds buffers, orders the nodes; GR_OK or an error
void gr_graph_run(gr_graph *g);     // one block of default_frames
// One block of `frames` frames (1 .. GR_MAX_FRAMES): the host's block size,
// which may change from block to block. GR_OK, or GR_ERR_ARG (nothing runs).
int gr_graph_run_frames(gr_graph *g, uint32_t frames);
// Changes the sample rate between blocks (nodes read b->sample_rate).
int gr_graph_set_sample_rate(gr_graph *g, uint32_t sample_rate);

const gr_events *gr_graph_events(const gr_graph *g, int node, int port);
const float *gr_graph_audio(const gr_graph *g, int node, int port);
const gr_timing *gr_graph_timing(const gr_graph *g, int node);
void gr_graph_reset_timing(gr_graph *g);

// The timing clock: ns on the host, CPU cycles on ESP32.
uint32_t gr_clock_ticks(void);

#ifdef __cplusplus
}
#endif

#endif
