// Routing nodes for the MIDI-hub use cases (spike, night 0, C1-2 prep), plus
// the edge nodes that move events between cores.
//
// Each node is a caller-owned struct and a const descriptor:
//   gr_transpose t; gr_transpose_init(&t, 12);
//   int id = gr_graph_add(&g, &gr_transpose_desc, &t);
// Parameters may be changed between blocks from the graph's own core. Notes
// remember how they were routed at note on, so a split, transpose or channel
// change mid-note never leaves a note stuck.
#ifndef GR_NODES_H
#define GR_NODES_H

#include <stdint.h>

#include "gr_graph.h"
#include "gr_spsc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GR_DROP 0xFF  // in a map: drop the message

// What crosses cores: an event stamped with its absolute frame.
typedef struct gr_qevent {
  uint64_t abs_frame;
  uint32_t ump[4];
} gr_qevent;

// Source: 0 in, 1 event out. Pops every queued event due before this block's
// end. An event already in the past lands on frame 0 and counts as late.
typedef struct gr_source {
  gr_spsc *q;
  uint32_t late;
  uint32_t taken;
} gr_source;
void gr_source_init(gr_source *n, gr_spsc *q);
extern const gr_node_desc gr_source_desc;

// Sink: 1 event in, 0 out. Pushes each event to a queue with its absolute frame.
typedef struct gr_sink {
  gr_spsc *q;
  uint32_t sent;
} gr_sink;
void gr_sink_init(gr_sink *n, gr_spsc *q);
extern const gr_node_desc gr_sink_desc;

// Merge: 2 or 4 event inputs, 1 out, in frame order (ties: lower input first).
typedef struct gr_merge {
  uint32_t merged;
} gr_merge;
extern const gr_node_desc gr_merge2_desc;
extern const gr_node_desc gr_merge4_desc;

// Split at a key: 1 in, 2 out (0 = below the key, 1 = the key and above).
// Messages that are not notes go to `other` (both, lower or upper).
enum { GR_SPLIT_BOTH = 0, GR_SPLIT_LOWER = 1, GR_SPLIT_UPPER = 2 };
typedef struct gr_split {
  uint8_t key;
  uint8_t other;
  uint32_t upper_on[16][4];  // per channel: notes sounding on the upper output
  uint32_t lower_on[16][4];
} gr_split;
void gr_split_init(gr_split *n, uint8_t key, uint8_t other);
extern const gr_node_desc gr_split_desc;

// Channel map: every channel voice message's channel through map[16]
// (GR_DROP drops it). System messages pass. Note offs follow their note on.
typedef struct gr_chanmap {
  uint8_t map[16];
  uint8_t note_ch[16][128];  // channel a sounding note went to, or GR_DROP
} gr_chanmap;
void gr_chanmap_init(gr_chanmap *n);  // identity
extern const gr_node_desc gr_chanmap_desc;

// Transpose notes (and poly pressure) by a signed number of semitones. Notes
// pushed past 0..127 are dropped, with their note off.
typedef struct gr_transpose {
  int8_t semitones;
  int8_t applied[16][128];  // offset used at note on; GR_TR_NONE if not sounding
  uint32_t out_of_range;
} gr_transpose;
#define GR_TR_NONE (-128)
#define GR_TR_DROPPED (-127)
void gr_transpose_init(gr_transpose *n, int semitones);
extern const gr_node_desc gr_transpose_desc;

// CC remap: controller numbers through map[128] (GR_DROP drops it), on the
// channels in channel_mask (bit n = channel n); other channels pass untouched.
typedef struct gr_ccremap {
  uint8_t map[128];
  uint16_t channel_mask;
} gr_ccremap;
void gr_ccremap_init(gr_ccremap *n);  // identity, all channels
extern const gr_node_desc gr_ccremap_desc;

// Impulse: 1 event in, 1 audio out. Writes the note on's velocity (0..1) at the
// note on's frame. A test node: proves audio ports and frame accuracy.
typedef struct gr_impulse {
  uint32_t hits;
} gr_impulse;
extern const gr_node_desc gr_impulse_desc;

// Null: 1 event in, 1 event out, copies. Measures the graph's own overhead.
extern const gr_node_desc gr_pass_desc;

#ifdef __cplusplus
}
#endif

#endif
