// Routing latency bench: does Wi-Fi or the web server slow Launchkey -> FM-1?
//
// Forward latency is from on_midi() entry (the controller's IN transfer has
// been dispatched on the USB client task) to the completion of the OUT
// transfer to the FM-1. Arrival jitter is how far the gaps between note-ons
// (an arpeggiator) and MIDI clock ticks stray from their median. A run records
// one mode for N seconds; the results of all modes are kept for comparison.
#pragma once

#include <stddef.h>
#include <stdint.h>

enum class BenchMode : uint8_t { None, WifiOff, Idle, Busy };
constexpr int kBenchModes = 4;

void bench_begin();

// USB client task. t_us from esp_timer_get_time() at on_midi() entry.
void bench_midi_in(int64_t t_us, uint8_t status, uint8_t data2);
// After midiSend() submitted a forwarded message that arrived at t_us.
void bench_forwarded(int64_t t_us);
// midiSend() refused a forwarded message (the '!' mark).
void bench_send_failed();

// Web server task: starts a run. False if one is running or the mode is bad.
bool bench_start(BenchMode mode, uint32_t seconds);
// Arduino loop: samples heap, ends the run. A wifi_off run starts recording
// 2 s after it is requested, once the web side has turned Wi-Fi off.
void bench_loop(uint32_t now_ms);
// The mode being recorded, None when idle.
BenchMode bench_running();

// JSON for every mode recorded so far.
size_t bench_json(char *out, size_t len);
// Raw samples of one mode as a JSON array: kind 0 forward, 1 note gaps, 2 clock gaps.
size_t bench_raw_json(BenchMode mode, int kind, char *out, size_t len);
const char *bench_mode_name(BenchMode mode);
BenchMode bench_mode_from(const char *name);
