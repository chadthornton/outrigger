#!/bin/sh
# The audio flow's static check (lib/graph/CONTRACT.md, rule 4): lists every
# object that references something core 1 must never call: an allocator,
# logging or stdio, a lock or a wait, a sleep, file I/O. Exit 1 if any does.
#   tools/graph_rt_symbols.sh [-n NM] obj.o...
# It checks whole objects, so keep a node's init (which may allocate) in the
# same object only if it calls nothing on this list either; the runtime
# checker (build/graph_check) catches allocation per call.
set -eu
NM=nm
if [ "${1:-}" = "-n" ]; then NM=$2; shift 2; fi
# Names as the linker sees them (an optional leading _ on the Mac).
BAD='malloc|calloc|realloc|free|aligned_alloc|posix_memalign|_Znwm|_Znam|_Znwj|_Znaj|_ZdlPv|_ZdaPv'
BAD="$BAD|printf|fprintf|vprintf|vfprintf|puts|fputs|putchar|fwrite|fflush|fopen|fclose|fread|write|read|open|close"
BAD="$BAD|pthread_mutex_lock|pthread_mutex_trylock|pthread_cond_wait|pthread_cond_timedwait|pthread_join|sem_wait|sched_yield"
BAD="$BAD|sleep|usleep|nanosleep|clock_nanosleep"
BAD="$BAD|xQueueSemaphoreTake|xQueueTakeMutexRecursive|xQueueReceive|xQueueGenericSend|vTaskDelay|vTaskDelayUntil|xTaskDelayUntil|ulTaskGenericNotifyTake|xEventGroupWaitBits"
BAD="$BAD|esp_log_write|esp_log_writev|ets_printf|esp_rom_printf|heap_caps_malloc|heap_caps_free"
status=0
for o in "$@"; do
  hits=$("$NM" -u "$o" 2>/dev/null | awk '{print $NF}' | sed 's/^_//' | grep -E "^($BAD)$" | tr '\n' ' ' || true)
  if [ -n "$hits" ]; then
    echo "audio flow must not call: $(basename "$o"): $hits"
    status=1
  fi
done
[ $status = 0 ] && echo "audio flow: $# objects, no allocator, log, lock, wait, sleep or file symbols"
exit $status
