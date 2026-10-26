// Spike: capture the FM-1's USB audio on the Waveshare ESP32-S3-Touch-LCD-1.69.
//
// Throwaway test firmware (branch spike/audio). It enumerates whatever is on
// the hub, shows each device's interface claims and the host channel estimate,
// then captures the FM-1's audio input (UAC1, 16-bit stereo, 44.1 kHz) in
// rolling 10 s windows into PSRAM and shows, per window: packets received vs.
// milliseconds elapsed (one iso packet per USB frame), frames vs. 44100/s,
// odd-sized packets, the longest gap between packet callbacks, and peak/RMS.
//
// Windows alternate between a frozen screen and a screen redrawn at 20 fps,
// to see whether drawing starves the USB client task.
//
// It never sends MIDI: there is no midiSend() call in this sketch.

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <EspUsbHost.h>
#include <esp_timer.h>
#include <math.h>

EspUsbHost usb;

// ---------- Display (pins from the main sketch's board_waveshare_169.cpp) ----------
constexpr int LCD_DC = 4, LCD_CS = 5, LCD_SCK = 6, LCD_MOSI = 7, LCD_RST = 8, LCD_BL = 15;
static Arduino_DataBus *bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCK, LCD_MOSI);
static Arduino_GFX *panel = new Arduino_ST7789(bus, LCD_RST, 0, true, 240, 280, 0, 20, 0, 20);
static Arduino_Canvas *gfx = new Arduino_Canvas(240, 280, panel);

// ---------- Capture state (written on the USB client task) ----------
constexpr uint32_t kRate = 44100;
constexpr uint32_t kWindowMs = 10000;
constexpr size_t kBufBytes = (kRate * kWindowMs / 1000 + kRate) * 4;  // a window plus 1 s of slack

static portMUX_TYPE cap_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t *pcm = nullptr;

struct WindowStats {
  uint32_t packets = 0, bytes = 0, odd_packets = 0;
  int64_t first_us = 0, last_us = 0, max_gap_us = 0;
  int32_t peak = 0;
  uint64_t sum_sq = 0;
  uint32_t samples = 0;
  uint32_t nonzero_packets = 0;
};
static volatile bool capturing = false;
static WindowStats live;               // under cap_lock
static uint32_t total_packets = 0;     // since start, under cap_lock
static uint8_t fm1_address = 0;
static volatile uint32_t changes = 0;

static bool is_fm1(uint16_t vid, uint16_t pid) {
  return (vid == 0x1209 && pid == 0x0001) || (vid == 0x4C4A && pid == 0xC755);
}

static void on_connected(const EspUsbHostDeviceInfo &d) {
  if (is_fm1(d.vid, d.pid)) fm1_address = d.address;
  ++changes;
}

static void on_disconnected(const EspUsbHostDeviceInfo &d) {
  if (d.address == fm1_address) fm1_address = 0;
  ++changes;
}

static void on_audio(const EspUsbHostAudioData &a) {
  const int64_t now = esp_timer_get_time();
  int32_t peak = 0;
  uint64_t sum_sq = 0;
  const int16_t *s = reinterpret_cast<const int16_t *>(a.data);
  const size_t n = a.length / 2;
  for (size_t i = 0; i < n; ++i) {
    const int32_t v = s[i];
    const int32_t m = v < 0 ? -v : v;
    if (m > peak) peak = m;
    sum_sq += static_cast<uint64_t>(v * v);
  }
  taskENTER_CRITICAL(&cap_lock);
  ++total_packets;
  if (capturing) {
    WindowStats &w = live;
    if (w.packets == 0) w.first_us = now;
    else if (now - w.last_us > w.max_gap_us) w.max_gap_us = now - w.last_us;
    w.last_us = now;
    ++w.packets;
    if (a.length != 176 && a.length != 180) ++w.odd_packets;
    if (peak) ++w.nonzero_packets;
    if (w.bytes + a.length <= kBufBytes) memcpy(pcm + w.bytes, a.data, a.length);
    w.bytes += a.length;
    if (peak > w.peak) w.peak = peak;
    w.sum_sq += sum_sq;
    w.samples += n;
  }
  taskEXIT_CRITICAL(&cap_lock);
}

// ---------- Facts shown on screen ----------
static char fm1_line[3][48];
static char dev_lines[6][48];
static int dev_line_count = 0;
static char status_line[48] = "waiting for the FM-1";
static size_t channels = 0;

struct WindowResult {
  bool live_screen;
  uint32_t packets, elapsed_ms, frames, odd, nonzero;
  uint32_t max_gap_ms;
  float peak_db, rms_db;
};
static WindowResult results[8];
static int result_count = 0;
static uint32_t window_no = 0;

static float to_db(double v) { return v > 0 ? 20.0f * log10f(static_cast<float>(v / 32768.0)) : -99.0f; }

static void gather_devices() {
  channels = usb.estimatedHcdChannelCount();
  dev_line_count = 0;
  EspUsbHostDeviceInfo devices[8];
  const size_t n = usb.getDevices(devices, 8);
  for (size_t i = 0; i < n && dev_line_count < 6; ++i) {
    const EspUsbHostDeviceInfo &d = devices[i];
    EspUsbHostInterfaceInfo itfs[10];
    const size_t ni = usb.getInterfaces(d.address, itfs, 10);
    // One letter per interface: claimed (C), tried and failed (F), not tried (-).
    char claims[12] = "";
    for (size_t k = 0; k < ni && k < 10; ++k)
      claims[k] = itfs[k].claimed ? 'C' : itfs[k].claimAttempted ? 'F' : '-';
    claims[ni < 10 ? ni : 10] = 0;
    snprintf(dev_lines[dev_line_count++], 48, "%04X:%04X ch%u %s %.8s", d.vid, d.pid,
             static_cast<unsigned>(usb.estimatedHcdChannelCount(d.address)), claims, d.product);
    if (is_fm1(d.vid, d.pid)) {
      snprintf(fm1_line[0], 48, "FM-1 bcd %04X  %s", d.deviceVersion, d.product);
      EspUsbHostAudioStreamInfo st[4];
      const size_t ns = usb.getAudioStreams(d.address, st, 4);
      fm1_line[1][0] = fm1_line[2][0] = 0;
      for (size_t k = 0; k < ns && k < 2; ++k)
        snprintf(fm1_line[1 + k], 48, "as if%u alt%u %s %uch %ub mp%u %s %lu/%lu", st[k].interfaceNumber,
                 st[k].alternate, st[k].input ? "IN" : "OUT", st[k].channels, st[k].bitsPerSample,
                 st[k].maxPacketSize, st[k].startable ? "ok" : "fmt",
                 static_cast<unsigned long>(st[k].sampleRateCount ? st[k].sampleRates[0] : st[k].sampleRate),
                 static_cast<unsigned long>(st[k].sampleRateCount > 1 ? st[k].sampleRates[1] : 0));
      if (ns == 0) strlcpy(fm1_line[1], "no audio streams", 48);
    }
  }
}

// ---------- Drawing ----------
static void draw(uint32_t now, bool recording, bool frozen) {
  gfx->fillScreen(0);
  gfx->setTextSize(1);
  int y = 22;
  auto line = [&](const char *t, uint16_t color = 0xFFFF, int x = 20) {
    gfx->setTextColor(color);
    gfx->setCursor(x, y);
    gfx->print(t);
    y += 10;
  };
  char buf[64];
  snprintf(buf, sizeof buf, "AUDIO SPIKE  ch %u/8", static_cast<unsigned>(channels));
  line(buf, 0x07FF, 40);
  for (int i = 0; i < dev_line_count; ++i) line(dev_lines[i], 0xC618, 6);
  for (auto &l : fm1_line) if (l[0]) line(l, 0xFFE0, 6);
  line(status_line, 0x07E0, 6);

  // Live meter of the current window.
  WindowStats w;
  uint32_t total;
  taskENTER_CRITICAL(&cap_lock);
  w = live;
  total = total_packets;
  taskEXIT_CRITICAL(&cap_lock);
  snprintf(buf, sizeof buf, "pkts total %lu", static_cast<unsigned long>(total));
  line(buf, 0xC618, 6);
  if (recording) {
    const float pk = to_db(w.peak);
    const float rms = w.samples ? to_db(sqrt(static_cast<double>(w.sum_sq) / w.samples)) : -99;
    snprintf(buf, sizeof buf, "W%lu %s pk %.0f rms %.0f dB", static_cast<unsigned long>(window_no),
             frozen ? "frozen" : "live", pk, rms);
    line(buf, 0xFFFF, 6);
    const int bar = pk <= -60 ? 0 : static_cast<int>((pk + 60) / 60 * 200);
    gfx->fillRect(20, y, bar, 6, pk > -1 ? 0xF800 : 0x07E0);
    y += 10;
  }
  y += 2;
  line("win scr  pkts/ms  frames odd gap pk rms", 0x07FF, 6);
  for (int i = 0; i < result_count; ++i) {
    const WindowResult &r = results[i];
    snprintf(buf, sizeof buf, "%c %5lu/%5lu %6lu %lu %2lu %3.0f %3.0f", r.live_screen ? 'L' : 'F',
             static_cast<unsigned long>(r.packets), static_cast<unsigned long>(r.elapsed_ms),
             static_cast<unsigned long>(r.frames), static_cast<unsigned long>(r.odd),
             static_cast<unsigned long>(r.max_gap_ms), r.peak_db, r.rms_db);
    line(buf, r.packets + 20 >= r.elapsed_ms ? 0xFFFF : 0xF800, 6);
  }
  gfx->flush();
}

// ---------- Capture control ----------
enum class Phase { Waiting, Starting, Recording, Failed };
static Phase phase = Phase::Waiting;
static uint32_t window_start_ms = 0, phase_at = 0;
static bool window_live = false;

static void begin_window(uint32_t now) {
  taskENTER_CRITICAL(&cap_lock);
  live = WindowStats{};
  capturing = true;
  taskEXIT_CRITICAL(&cap_lock);
  window_start_ms = now;
  window_live = (window_no % 2) == 1;  // first window frozen, then alternate
  ++window_no;
}

static void end_window(uint32_t now) {
  taskENTER_CRITICAL(&cap_lock);
  capturing = false;
  const WindowStats w = live;
  taskEXIT_CRITICAL(&cap_lock);
  WindowResult r{};
  r.live_screen = window_live;
  r.packets = w.packets;
  r.elapsed_ms = now - window_start_ms;
  r.frames = w.bytes / 4;
  r.odd = w.odd_packets;
  r.nonzero = w.nonzero_packets;
  r.max_gap_ms = static_cast<uint32_t>(w.max_gap_us / 1000);
  r.peak_db = to_db(w.peak);
  r.rms_db = w.samples ? to_db(sqrt(static_cast<double>(w.sum_sq) / w.samples)) : -99;
  if (result_count == 8) {
    memmove(results, results + 1, sizeof results[0] * 7);
    result_count = 7;
  }
  results[result_count++] = r;
}

void setup() {
  panel->begin(80000000);
  gfx->begin(GFX_SKIP_OUTPUT_BEGIN);
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH);

  pcm = static_cast<uint8_t *>(ps_malloc(kBufBytes));
  if (!pcm) strlcpy(status_line, "PSRAM alloc failed", sizeof status_line);

  usb.onDeviceConnected(on_connected);
  usb.onDeviceDisconnected(on_disconnected);
  usb.onAudioData(on_audio);
  usb.begin();
}

void loop() {
  static uint32_t last_draw = 0, last_gather = 0, seen_changes = 0;
  const uint32_t now = millis();

  if (changes != seen_changes || now - last_gather > 2000) {
    seen_changes = changes;
    last_gather = now;
    gather_devices();
  }

  switch (phase) {
    case Phase::Waiting:
      if (pcm && fm1_address && usb.audioInputReady(fm1_address)) {
        phase = Phase::Starting;
        phase_at = now;
      }
      break;
    case Phase::Starting:
      if (now - phase_at < 1500) break;  // let enumeration settle
      if (usb.audioInputStart(2, 16, kRate, fm1_address)) {
        strlcpy(status_line, "capturing 44.1k 16b stereo", sizeof status_line);
        phase = Phase::Recording;
        begin_window(now);
      } else {
        snprintf(status_line, sizeof status_line, "start failed: %s", usb.lastErrorName());
        phase = Phase::Failed;
        phase_at = now;
      }
      break;
    case Phase::Recording:
      if (!fm1_address) {
        taskENTER_CRITICAL(&cap_lock);
        capturing = false;
        taskEXIT_CRITICAL(&cap_lock);
        strlcpy(status_line, "FM-1 gone", sizeof status_line);
        phase = Phase::Waiting;
      } else if (now - window_start_ms >= kWindowMs) {
        end_window(now);
        begin_window(now);
      }
      break;
    case Phase::Failed:
      if (now - phase_at > 5000 && fm1_address) phase = Phase::Starting;
      break;
  }

  const bool recording = phase == Phase::Recording;
  const bool frozen = recording && !window_live;
  // Frozen windows draw once at their start (so the header shows), then not again.
  static uint32_t drawn_window = UINT32_MAX;
  if (frozen) {
    if (drawn_window != window_no) {
      drawn_window = window_no;
      draw(now, recording, frozen);
    }
  } else if (now - last_draw > 50) {
    last_draw = now;
    drawn_window = window_no;
    draw(now, recording, frozen);
  }
  delay(2);
}
