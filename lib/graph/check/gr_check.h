// The app checker (spike, night 1, track S2): checks a node or an app against
// lib/graph/CONTRACT.md before it runs, the way VIBE's vibe-check checks a
// Vibe. Host only (it allocates and forks); never part of a firmware.
//
// A subject is either one node (desc != NULL) or an app (build != NULL) that
// adds nodes to a graph between the checker's feeds and its outputs. The
// checker drives it with fuzzed, seeded MIDI 1.0 / 2.0 / system / SysEx UMPs
// at block sizes 1..256 and three sample rates, and reports one line per
// check: PASS, FAIL, or ---- (not tested, with why).
#ifndef GR_CHECK_H
#define GR_CHECK_H

#include <stddef.h>
#include <stdint.h>

#include "gr_graph.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
  GR_CK_BLOCK_DEPENDENT = 1u << 0,  // output may depend on block sizes (declared, reported, not failed)
  GR_CK_NOTES = 1u << 1,            // passes notes: every note on must get its off
  GR_CK_AUDIO_IN = 1u << 2,         // feed its audio inputs (a deterministic signal)
  GR_CK_CLOCKED = 1u << 3,          // its time moves only with MIDI clock: the input ends with a Stop
};

#define GR_CK_MAX_OUT 8

typedef struct gr_ck_subject {
  const char *name;
  uint32_t flags;
  // One node:
  const gr_node_desc *desc;
  size_t self_size;
  int (*init)(void *self);  // 1 = ready (control flow: may allocate)
  void (*fini)(void *self);
  // Or an app: adds nodes after the feeds (feed[i] is the node feeding event
  // input i, port 0), fills its outputs, returns how many (or < 0).
  int n_inputs;
  size_t mem_size;
  int (*build)(gr_graph *g, void *mem, const int *feed, int *out_node, int *out_port);
  // Either: a parameter change between blocks (the stuck-note run). mem is
  // self for a node.
  void (*twiddle)(void *mem, uint32_t rnd);
  // Checks known to fail, with why ("Notes: ..."): reported as KNOWN, not
  // counted as failures. For third-party code we test but do not change.
  const char *known;
} gr_ck_subject;

typedef struct gr_ck_options {
  double seconds;     // of fuzzed input per block plan (default 2)
  uint32_t seed;      // default 1
  int isolate;        // run in a child process, so a crash is a FAIL, not the end
  unsigned timeout_s; // with isolate: a hang is a FAIL (default 60)
} gr_ck_options;

typedef struct gr_ck_line {
  char part[16];
  int ok;  // 1 pass, 0 fail, -1 not tested, -2 a known failure (gr_ck_subject.known)
  char text[200];
} gr_ck_line;

typedef struct gr_ck_report {
  char name[48];
  int n, fails;
  gr_ck_line line[32];
} gr_ck_report;

void gr_ck_defaults(gr_ck_options *o);
// Runs every check; returns the number of failures (also in r->fails).
int gr_ck_run(const gr_ck_subject *s, const gr_ck_options *o, gr_ck_report *r);
// Did the check `part` fail? (for tests)
int gr_ck_failed(const gr_ck_report *r, const char *part);

// Allocation counter the checked code is built against (-Dmalloc=gr_count_malloc
// and friends, tests/support/gr_alloc_count.c).
extern unsigned long gr_alloc_calls;

#ifdef __cplusplus
}
#endif

#endif
