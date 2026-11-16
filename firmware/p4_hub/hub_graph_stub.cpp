#include "hub_graph.h"

#include <stdio.h>

void graph_begin() {}

size_t graph_state_json(char *out, size_t cap) {
  const int n = snprintf(out, cap,
                         "{\"graph\":\"stub\",\"nodes\":[],\"edges\":[],"
                         "\"note\":\"HW1 stub: HW2 puts track D's graph here\"}");
  return n < 0 ? 0 : ((size_t)n < cap ? (size_t)n : cap - 1);
}
