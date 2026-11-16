// The graph's state for GET /graph. Tonight (HW1) a stub; HW2 replaces
// hub_graph_stub.cpp with track D's graph (spike/platform-p4-d-graph) and
// keeps this interface: graph_begin() once in setup(), graph_state_json()
// from the web server's task (so it must only read published copies).
#pragma once
#include <stddef.h>

void graph_begin();
size_t graph_state_json(char *out, size_t cap);
