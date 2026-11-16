#include "gr_swap.h"

void gr_swap_init(gr_swap *s, void *first, void *second) {
  s->copy[0] = first;
  s->copy[1] = second;
  s->published = 0;
  s->taken = 0;
}

void *gr_swap_back(gr_swap *s) {
  uint32_t p = __atomic_load_n(&s->published, __ATOMIC_RELAXED);
  // The audio flow has acknowledged p, so it reads copy[p & 1] and never the other.
  if (__atomic_load_n(&s->taken, __ATOMIC_ACQUIRE) != p) return 0;
  return s->copy[(p + 1) & 1];
}

int gr_swap_publish(gr_swap *s) {
  if (!gr_swap_back(s)) return 0;
  uint32_t p = __atomic_load_n(&s->published, __ATOMIC_RELAXED);
  __atomic_store_n(&s->published, p + 1, __ATOMIC_RELEASE);
  return 1;
}

const void *gr_swap_take(gr_swap *s) {
  uint32_t p = __atomic_load_n(&s->published, __ATOMIC_ACQUIRE);
  if (p != __atomic_load_n(&s->taken, __ATOMIC_RELAXED))
    __atomic_store_n(&s->taken, p, __ATOMIC_RELEASE);  // done with the old copy
  return s->copy[p & 1];
}
