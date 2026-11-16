// Schwung MIDI-effect shim (spike, night 0, milestone C1-4).
//
// Runs an unmodified Schwung midi_fx module (src/host/midi_fx_api_v1.h:
// create_instance / process_midi / tick / set_param / get_param) as one node
// of our graph: 1 event in, 1 event out.
//
// Per block, the shim walks the input in frame order. Before each event it
// calls tick() for the frames since the last one (split further into
// `tick_quantum`-frame steps), then hands the event to process_midi() as
// MIDI 1.0 bytes. Output from process_midi lands on the event's frame; output
// from tick lands on the first frame of the tick's span. With tick_quantum
// equal to the block size and every input on frame 0, this is exactly what
// Schwung's own chain host does (all of a block's MIDI, then one tick).
//
// The note guard: the shim tracks the notes the module has sounding, and on
// Start, Continue or Stop ends any the module left on (graph_check found
// both modules resetting on transport without their note offs).
//
// UMPs with no MIDI 1.0 form (SysEx, utility, per-note MIDI 2.0) pass
// through untouched. The module's output is converted back to UMP as MIDI 1.0
// (type 2) or MIDI 2.0 (type 4) protocol.
//
// create_instance allocates (Schwung modules calloc their state), so it runs
// in gr_schwung_init, never on the process path.
#ifndef GR_SCHWUNG_H
#define GR_SCHWUNG_H

#include <stdint.h>

#include "gr_graph.h"

#ifdef __cplusplus
extern "C" {
#endif

struct midi_fx_api_v1;
struct host_api_v1;
typedef struct midi_fx_api_v1 *(*gr_midi_fx_init_fn)(const struct host_api_v1 *host);

typedef struct gr_schwung {
  const struct midi_fx_api_v1 *api;
  void *inst;
  uint8_t group;       // UMP group for output
  uint8_t out_proto2;  // 1: emit MIDI 2.0 protocol, 0: MIDI 1.0
  uint32_t tick_quantum;  // frames per tick() call, 1 .. block frames
  uint32_t truncated;  // module output dropped because max_out (16) filled
  uint32_t passed;     // UMPs passed through without the module
  uint32_t on[16][4];  // the note guard: the module's notes now sounding
  uint32_t guard_offs; // note offs the guard sent for a module that reset without them
} gr_schwung;

// init is the module's move_midi_fx_init (renamed at build time so several
// modules can link into one binary). module_dir may be NULL. Returns 1 on success.
int gr_schwung_init(gr_schwung *n, gr_midi_fx_init_fn init, const char *module_dir,
                    uint32_t tick_quantum, int out_proto2);
void gr_schwung_destroy(gr_schwung *n);
void gr_schwung_set(gr_schwung *n, const char *key, const char *val);
int gr_schwung_get(gr_schwung *n, const char *key, char *buf, int len);

// The host API the modules see: clock status follows the MIDI transport the
// shim has seen (start/continue -> running, stop -> stopped), shared by every
// shim instance in the process, as in Schwung. bpm is the host tempo.
void gr_schwung_set_bpm(float bpm);
// Back to stopped (the transport is process-wide: a new graph starts here).
void gr_schwung_reset_transport(void);

extern const gr_node_desc gr_schwung_desc;

#ifdef __cplusplus
}
#endif

#endif
