// Lock-free single-producer single-consumer ring for core-to-core traffic
// (core 0's USB task -> core 1's graph, and back). Fixed-size elements in a
// caller-owned buffer; capacity is a power of two. Wait-free on both sides,
// no allocation, no locks. Uses the GCC/Clang __atomic builtins so the same
// code builds as C or C++ for the host, Xtensa (S3) and RISC-V (P4).
//
// head is written only by the producer, tail only by the consumer. Each is
// on its own cache line so the two cores do not false-share.
#ifndef GR_SPSC_H
#define GR_SPSC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GR_CACHE_LINE 64

typedef struct gr_spsc {
  uint32_t head;  // next slot the producer writes (producer-owned)
  uint8_t pad0[GR_CACHE_LINE - sizeof(uint32_t)];
  uint32_t tail;  // next slot the consumer reads (consumer-owned)
  uint8_t pad1[GR_CACHE_LINE - sizeof(uint32_t)];
  uint32_t mask;
  uint32_t elem_size;
  uint8_t *buf;
  uint32_t dropped;  // pushes refused because the ring was full (producer-owned)
} gr_spsc;

// capacity must be a power of two; buf holds capacity * elem_size bytes.
int gr_spsc_init(gr_spsc *q, void *buf, uint32_t capacity, uint32_t elem_size);

// Producer side. Returns 1 if queued, 0 if full (and counts a drop).
int gr_spsc_push(gr_spsc *q, const void *elem);

// Consumer side. peek returns the oldest element without removing it (NULL if
// empty); pop removes it, copying it out if out is non-NULL. Returns 1/0.
const void *gr_spsc_peek(gr_spsc *q);
int gr_spsc_pop(gr_spsc *q, void *out);

uint32_t gr_spsc_count(const gr_spsc *q);  // a snapshot; exact only on one side

#ifdef __cplusplus
}
#endif

#endif
