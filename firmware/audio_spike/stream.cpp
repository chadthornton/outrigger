// Wi-Fi WAV stream for the audio spike.
//
// The USB audio callback pushes every packet into a PSRAM ring. A task serves
// GET /take.wav?s=N on port 80: a WAV header sized for N seconds (default 30),
// then the audio from the moment of the request, read from the ring. Wi-Fi
// stalls only cost ring space; if a reader falls a whole ring behind it skips
// ahead and counts an overrun.
//
// Joins the network the web-UI spike saved in NVS ("wifi" namespace), so there
// is no setup page. Hostname fm1audio(.local).

#include "stream.h"

#include <Arduino.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFi.h>

constexpr size_t kRingBytes = 4 * 1024 * 1024;  // about 23 s at 176.4 kB/s
constexpr uint32_t kRate = 44100;
constexpr uint32_t kBytesPerSec = kRate * 4;

static uint8_t *ring = nullptr;
static portMUX_TYPE ring_lock = portMUX_INITIALIZER_UNLOCKED;
static uint64_t written = 0;  // total bytes pushed, under ring_lock

static char ssid[33], pass[65];
static WiFiServer server(80);

struct StreamStats {
  uint32_t takes = 0, overruns = 0, max_stall_ms = 0;
  uint64_t sent = 0;
  bool client = false;
  uint32_t last_kbps = 0;
};
static StreamStats stats;  // written by the stream task only; read for display

static uint64_t written_now() {
  taskENTER_CRITICAL(&ring_lock);
  const uint64_t w = written;
  taskEXIT_CRITICAL(&ring_lock);
  return w;
}

void stream_push(const uint8_t *data, size_t len) {
  if (!ring) return;
  taskENTER_CRITICAL(&ring_lock);
  const size_t at = written % kRingBytes;
  taskEXIT_CRITICAL(&ring_lock);
  const size_t first = min(len, kRingBytes - at);
  memcpy(ring + at, data, first);
  if (first < len) memcpy(ring, data + first, len - first);
  taskENTER_CRITICAL(&ring_lock);
  written += len;
  taskEXIT_CRITICAL(&ring_lock);
}

static void put_le(uint8_t *p, uint32_t v, int n) {
  for (int i = 0; i < n; ++i) p[i] = (v >> (8 * i)) & 0xFF;
}

static void wav_header(uint8_t h[44], uint32_t data_bytes) {
  memcpy(h, "RIFF", 4);
  put_le(h + 4, 36 + data_bytes, 4);
  memcpy(h + 8, "WAVEfmt ", 8);
  put_le(h + 16, 16, 4);
  put_le(h + 20, 1, 2);  // PCM
  put_le(h + 22, 2, 2);
  put_le(h + 24, kRate, 4);
  put_le(h + 28, kBytesPerSec, 4);
  put_le(h + 32, 4, 2);
  put_le(h + 34, 16, 2);
  memcpy(h + 36, "data", 4);
  put_le(h + 40, data_bytes, 4);
}

static void serve(WiFiClient &c) {
  // Request line only; headers are read and ignored.
  String line = c.readStringUntil('\n');
  while (c.connected()) {
    String h = c.readStringUntil('\n');
    if (h.length() <= 1) break;
  }
  if (!line.startsWith("GET /take.wav")) {
    c.print("HTTP/1.0 404 Not Found\r\nContent-Type: text/plain\r\n\r\nGET /take.wav?s=30\n");
    return;
  }
  uint32_t secs = 30;
  const int q = line.indexOf("s=");
  if (q > 0) secs = constrain(line.substring(q + 2).toInt(), 1, 3600);
  const uint32_t data_bytes = secs * kBytesPerSec;

  c.setNoDelay(true);
  c.printf("HTTP/1.0 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: %lu\r\n\r\n",
           static_cast<unsigned long>(44 + data_bytes));
  uint8_t h[44];
  wav_header(h, data_bytes);
  c.write(h, sizeof h);

  ++stats.takes;
  stats.client = true;
  uint64_t pos = written_now() & ~uint64_t{3};
  uint32_t left = data_bytes;
  const uint32_t started = millis();
  uint32_t last_progress = started;
  while (left && c.connected()) {
    uint64_t avail = written_now() - pos;
    if (avail > kRingBytes - 64 * 1024) {  // a whole ring behind: skip to fresh data
      ++stats.overruns;
      pos = (written_now() - 64 * 1024) & ~uint64_t{3};
      avail = written_now() - pos;
    }
    if (avail == 0) {
      delay(5);
      continue;
    }
    size_t n = min<uint64_t>(min<uint64_t>(avail, 4096), left);
    const size_t at = pos % kRingBytes;
    n = min(n, kRingBytes - at);
    const size_t sent = c.write(ring + at, n);
    const uint32_t now = millis();
    if (sent == 0) {
      if (now - last_progress > 5000) break;  // the client is gone
      delay(2);
      continue;
    }
    if (now - last_progress > stats.max_stall_ms) stats.max_stall_ms = now - last_progress;
    last_progress = now;
    pos += sent;
    left -= sent;
    stats.sent += sent;
    if (now > started) stats.last_kbps = static_cast<uint32_t>((data_bytes - left) * 8ULL / (now - started));
  }
  stats.client = false;
}

static void stream_task(void *) {
  WiFi.persistent(false);
  WiFi.setHostname("fm1audio");
  WiFi.mode(WIFI_STA);
  WiFi.enableIPv6(true);  // link-local: mDNS answers the AAAA query macOS sends first
  WiFi.begin(ssid, pass);
  while (WiFi.status() != WL_CONNECTED) delay(200);
  MDNS.begin("fm1audio");
  MDNS.addService("http", "tcp", 80);
  server.begin();
  for (;;) {
    WiFiClient c = server.available();
    if (c) {
      serve(c);
      c.stop();
    }
    delay(10);
  }
}

bool stream_begin() {
  ring = static_cast<uint8_t *>(ps_malloc(kRingBytes));
  Preferences p;
  p.begin("wifi", true);
  strlcpy(ssid, p.getString("ssid", "").c_str(), sizeof ssid);
  strlcpy(pass, p.getString("pass", "").c_str(), sizeof pass);
  p.end();
  if (!ring || !ssid[0]) return false;
  // Core 0 with the Wi-Fi stack; the USB client task and loop() stay free.
  xTaskCreatePinnedToCore(stream_task, "wav", 6144, nullptr, 3, nullptr, 0);
  return true;
}

void stream_status(char *out, size_t len) {
  if (!ring) {
    strlcpy(out, "wifi: no PSRAM ring", len);
  } else if (!ssid[0]) {
    strlcpy(out, "wifi: no saved network", len);
  } else if (WiFi.status() != WL_CONNECTED) {
    snprintf(out, len, "wifi: joining %.16s", ssid);
  } else {
    snprintf(out, len, "%s %s", WiFi.localIP().toString().c_str(), stats.client ? "SENDING" : "idle");
  }
}

void stream_stats(char *out, size_t len) {
  snprintf(out, len, "takes %lu %luk %lukbps ovr %lu st %lums", static_cast<unsigned long>(stats.takes),
           static_cast<unsigned long>(stats.sent / 1024), static_cast<unsigned long>(stats.last_kbps),
           static_cast<unsigned long>(stats.overruns), static_cast<unsigned long>(stats.max_stall_ms));
}
