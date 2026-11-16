// Positive control for tools/graph_rt_symbols.sh: a node that logs, locks
// and allocates in its process call. Built only to be rejected.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
void gr_rt_bad_process(void) {
  pthread_mutex_lock(&m);
  void *p = malloc(16);
  printf("block %p\n", p);
  free(p);
  pthread_mutex_unlock(&m);
}
