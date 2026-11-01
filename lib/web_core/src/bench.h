// The test bench's control API (BENCH=1 builds only, never a release): the Mac
// (tools/bench.py) starts a firmware switch or a backup on a Waveshare wired
// to FM-1-2, with nobody at the screen. Everything here is pure, so the Mac
// tests hold it; the routes are registered only when the board's MAC is one of
// Chad's Waveshares (waveshare_name), and the Mac checks it's the one it asked for.
//
// The API presses what the screen would press: select a row (its plan is
// previewed), then act on it. The Loader still refuses an act whose row
// doesn't match its last preview, so the Mac checks the plan in /status
// between the two.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "routes.h"

namespace web_core {

constexpr Route kBenchRoutes[] = {
    {"/bench/state", Method::Get, false},    // JSON: MAC verdict, boot count, last command and its answer
    {"/bench/select", Method::Post, false},  // ?index=N: open row N's plan, as tapping it does
    {"/bench/act", Method::Post, false},     // ?kind=switch|backup|retry[&index=N]: the 2-second hold
};
constexpr int kBenchRouteCount = sizeof kBenchRoutes / sizeof kBenchRoutes[0];

// Chad's Waveshares by MAC ("waveshare-1", "waveshare-2"); null for any other
// board, which gets no bench API.
const char *waveshare_name(const uint8_t mac[6]);

enum class BenchKind : uint8_t { None, Select, Switch, Backup, Retry };
struct BenchCommand {
  BenchKind kind = BenchKind::None;
  int index = -1;  // the row, for Select and Switch
};

// path: "/bench/select" or "/bench/act"; query: the part after '?', or "".
// False with *why set when the request names nothing the bench can do.
bool parse_bench_command(const char *path, const char *query, BenchCommand &out, const char **why);
const char *bench_kind_name(BenchKind k);

}  // namespace web_core
