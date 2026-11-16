// Counting allocator for the Schwung modules in the graph tests: the module
// sources are built with -Dmalloc=gr_count_malloc (and calloc, free), so a
// test can prove that nothing allocates once the graph is running.
#include <stdlib.h>

unsigned long gr_alloc_calls = 0;

void *gr_count_malloc(size_t n) {
  ++gr_alloc_calls;
  return malloc(n);
}

void *gr_count_calloc(size_t n, size_t size) {
  ++gr_alloc_calls;
  return calloc(n, size);
}

void gr_count_free(void *p) {
  if (p) ++gr_alloc_calls;
  free(p);
}
