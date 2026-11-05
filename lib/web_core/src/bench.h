// The test bench's control API (BENCH=1 builds only, never a release): the Mac
// (tools/bench.py) starts a firmware switch or a backup on a Waveshare wired
// to a bench FM-1, with nobody at the screen. Everything here is pure, so the Mac
// tests hold it; the routes are registered only when the board's MAC is in the
// device registry the build was made with (waveshare_name), and the Mac checks
// it's the one it asked for.
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
    {"/bench/update", Method::Post, false},  // ?sha256=&label=[&slot=recovery], body: the image (docs/ota-updates.md)
};
constexpr int kBenchRouteCount = sizeof kBenchRoutes / sizeof kBenchRoutes[0];

// The registry's boards by MAC ("small-v1", "small-v2", "big-s3"); null for any
// other board, which gets no bench API. Release builds list none.
const char *waveshare_name(const uint8_t mac[6]);

// The board's network name: its name above when listed ("small-v1", so each
// test board answers at its own .local address), else the build's default
// ("fm1bench" in a BENCH=1 build, "fm1hub" otherwise). Copied into out.
void web_host_name(const uint8_t mac[6], bool bench_build, char *out, size_t len);

enum class BenchKind : uint8_t { None, Select, Switch, Backup, Retry };
struct BenchCommand {
  BenchKind kind = BenchKind::None;
  int index = -1;  // the row, for Select and Switch
};

// path: "/bench/select" or "/bench/act"; query: the part after '?', or "".
// False with *why set when the request names nothing the bench can do.
bool parse_bench_command(const char *path, const char *query, BenchCommand &out, const char **why);

// POST /bench/update: an image for app0 (the update runs through the recovery
// app) or for the recovery slot itself (written in place).
enum class UpdateSlot : uint8_t { App, Recovery };
struct BenchUpdate {
  UpdateSlot slot = UpdateSlot::App;
  uint32_t size = 0;  // the body's Content-Length
  uint8_t sha256[32] = {};
  char label[16] = {};  // the build's commit, for /status (letters, digits, '+', '-', '.')
};
// query: "sha256=<64 hex>&label=<commit>[&slot=app|recovery]". length: the
// body's Content-Length; slot_bytes: that slot's size (app0 or recovery). An
// ESP32 image is at least a header, so anything under 256 bytes is refused.
bool parse_bench_update(const char *query, size_t length, size_t app_bytes, size_t recovery_bytes, BenchUpdate &out,
                        const char **why);
const char *bench_kind_name(BenchKind k);

}  // namespace web_core
