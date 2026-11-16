#include "gr_spsc.h"

#include <string.h>

int gr_spsc_init(gr_spsc *q, void *buf, uint32_t capacity, uint32_t elem_size) {
  if (!q || !buf || capacity < 2 || (capacity & (capacity - 1)) || elem_size == 0) return 0;
  memset(q, 0, sizeof(*q));
  q->mask = capacity - 1;
  q->elem_size = elem_size;
  q->buf = (uint8_t *)buf;
  return 1;
}

int gr_spsc_push(gr_spsc *q, const void *elem) {
  uint32_t head = __atomic_load_n(&q->head, __ATOMIC_RELAXED);
  uint32_t tail = __atomic_load_n(&q->tail, __ATOMIC_ACQUIRE);
  if (head - tail > q->mask) {
    q->dropped++;
    return 0;
  }
  memcpy(q->buf + (size_t)(head & q->mask) * q->elem_size, elem, q->elem_size);
  __atomic_store_n(&q->head, head + 1, __ATOMIC_RELEASE);  // publish the element
  return 1;
}

const void *gr_spsc_peek(gr_spsc *q) {
  uint32_t tail = __atomic_load_n(&q->tail, __ATOMIC_RELAXED);
  uint32_t head = __atomic_load_n(&q->head, __ATOMIC_ACQUIRE);
  if (head == tail) return NULL;
  return q->buf + (size_t)(tail & q->mask) * q->elem_size;
}

int gr_spsc_pop(gr_spsc *q, void *out) {
  uint32_t tail = __atomic_load_n(&q->tail, __ATOMIC_RELAXED);
  uint32_t head = __atomic_load_n(&q->head, __ATOMIC_ACQUIRE);
  if (head == tail) return 0;
  if (out) memcpy(out, q->buf + (size_t)(tail & q->mask) * q->elem_size, q->elem_size);
  __atomic_store_n(&q->tail, tail + 1, __ATOMIC_RELEASE);  // hand the slot back
  return 1;
}

uint32_t gr_spsc_count(const gr_spsc *q) {
  return __atomic_load_n(&q->head, __ATOMIC_ACQUIRE) - __atomic_load_n(&q->tail, __ATOMIC_ACQUIRE);
}
