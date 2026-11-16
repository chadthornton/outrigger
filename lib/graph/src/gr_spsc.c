#include "gr_spsc.h"

#include <string.h>

// Positive control for `make graph-tsan` only: weakens the handover to
// relaxed, which TSan must report as a race on the element. Never set it in
// a build that ships.
#ifdef GR_SPSC_TEST_RELAXED
#define GR_PUBLISH __ATOMIC_RELAXED
#define GR_OBSERVE __ATOMIC_RELAXED
#else
#define GR_PUBLISH __ATOMIC_RELEASE
#define GR_OBSERVE __ATOMIC_ACQUIRE
#endif

#ifdef GR_CHECK_FLOWS
#if defined(ESP_PLATFORM)
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
uintptr_t gr_flow_self(void) { return (uintptr_t)xTaskGetCurrentTaskHandle(); }
#else
#include <pthread.h>
uintptr_t gr_flow_self(void) { return (uintptr_t)pthread_self(); }
#endif
// Records the first caller of a side, and counts any other caller after it.
static void flow_check(uintptr_t *owner, uint32_t *violations) {
  uintptr_t me = gr_flow_self(), expected = 0;
  if (!__atomic_compare_exchange_n(owner, &expected, me, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED) &&
      expected != me)
    __atomic_fetch_add(violations, 1, __ATOMIC_RELAXED);
}
#define FLOW_PRODUCER(q) flow_check(&(q)->producer, &(q)->producer_violations)
#define FLOW_CONSUMER(q) flow_check(&(q)->consumer, &(q)->consumer_violations)
#else
#define FLOW_PRODUCER(q) ((void)0)
#define FLOW_CONSUMER(q) ((void)0)
#endif

int gr_spsc_init(gr_spsc *q, void *buf, uint32_t capacity, uint32_t elem_size) {
  if (!q || !buf || capacity < 2 || (capacity & (capacity - 1)) || elem_size == 0) return 0;
  memset(q, 0, sizeof(*q));
  q->mask = capacity - 1;
  q->elem_size = elem_size;
  q->buf = (uint8_t *)buf;
  return 1;
}

int gr_spsc_push(gr_spsc *q, const void *elem) {
  FLOW_PRODUCER(q);
  uint32_t head = __atomic_load_n(&q->head, __ATOMIC_RELAXED);
  uint32_t tail = __atomic_load_n(&q->tail, GR_OBSERVE);
  if (head - tail > q->mask) {
    // One writer (this side); others may read it, so it is an atomic value.
    __atomic_store_n(&q->dropped, __atomic_load_n(&q->dropped, __ATOMIC_RELAXED) + 1, __ATOMIC_RELAXED);
    return 0;
  }
  memcpy(q->buf + (size_t)(head & q->mask) * q->elem_size, elem, q->elem_size);
  __atomic_store_n(&q->head, head + 1, GR_PUBLISH);  // publish the element
  return 1;
}

const void *gr_spsc_peek(gr_spsc *q) {
  FLOW_CONSUMER(q);
  uint32_t tail = __atomic_load_n(&q->tail, __ATOMIC_RELAXED);
  uint32_t head = __atomic_load_n(&q->head, GR_OBSERVE);
  if (head == tail) return NULL;
  return q->buf + (size_t)(tail & q->mask) * q->elem_size;
}

int gr_spsc_pop(gr_spsc *q, void *out) {
  FLOW_CONSUMER(q);
  uint32_t tail = __atomic_load_n(&q->tail, __ATOMIC_RELAXED);
  uint32_t head = __atomic_load_n(&q->head, GR_OBSERVE);
  if (head == tail) return 0;
  if (out) memcpy(out, q->buf + (size_t)(tail & q->mask) * q->elem_size, q->elem_size);
  __atomic_store_n(&q->tail, tail + 1, GR_PUBLISH);  // hand the slot back
  return 1;
}

uint32_t gr_spsc_dropped(const gr_spsc *q) { return __atomic_load_n(&q->dropped, __ATOMIC_RELAXED); }

uint32_t gr_spsc_count(const gr_spsc *q) {
  return __atomic_load_n(&q->head, __ATOMIC_ACQUIRE) - __atomic_load_n(&q->tail, __ATOMIC_ACQUIRE);
}
