// Swap with acknowledge (CONTRACT.md, rule 3): hands big state (a routing
// table, a scene, a pattern) from the control flow to the audio flow without
// a lock and without either side waiting.
//
//   Control flow:  T *next = gr_swap_back(&s);        // NULL: the last one is not taken yet
//                  fill(next); gr_swap_publish(&s);   // release: the data, then the counter
//   Audio flow:    const T *cur = gr_swap_take(&s);   // at a good moment (a block start)
//                                                     // acquire: the counter, then the data
//
// `published` has one writer (control), `taken` one writer (audio). The
// control flow writes only the copy the audio flow no longer reads.
#ifndef GR_SWAP_H
#define GR_SWAP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gr_swap {
  void *copy[2];
  uint32_t published;  // control flow's counter
  uint32_t taken;      // audio flow's acknowledge
} gr_swap;

// Both copies caller-owned; `first` is in force from the start (copy[0]).
void gr_swap_init(gr_swap *s, void *first, void *second);
// Control flow: the copy to fill next, or NULL while the audio flow has not
// taken the last one (try again later; never wait).
void *gr_swap_back(gr_swap *s);
// Control flow: makes the filled back copy current. 0 if it was not free.
int gr_swap_publish(gr_swap *s);
// Audio flow: the copy in force, switching to a newly published one.
const void *gr_swap_take(gr_swap *s);

#ifdef __cplusplus
}
#endif

#endif
