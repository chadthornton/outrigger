// vh_native_host.c: the three host calls a natively compiled Vibe makes (the .wasm imports them from "env").
#include "vh.h"
#include <stdint.h>

vh_log_fn vh_native_log;   // set by the bench; 0 = drop

void vibe_host_log(const char *s, int len) { if (vh_native_log) vh_native_log(s, len); }
void vibe_host_request(int what) { (void)what; }
int vibe_host_store(int op, const char *name, int name_len, void *data, int len)
{
  (void)op; (void)name; (void)name_len; (void)data; (void)len;
  return -1;   // no storage on the bench
}
