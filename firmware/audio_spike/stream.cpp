// Wi-Fi WAV stream for the audio spike.
//
// The USB audio callback pushes every packet into a PSRAM ring. A task serves
// GET /take.wav?s=N on port 80: a WAV header sized for N seconds (default 30),
// then the audio from the moment of the request. While a take runs, the ring
// stops filling at the take's end, so nothing the reader still needs is ever
// overwritten: a slow link makes the download slower, never the audio
// discontinuous. N is capped at what the ring holds. GET /stats: counters.
//
// Joins the network the web-UI spike saved in NVS ("wifi" namespace), so there
// is no setup page. Hostname fm1audio(.local). POST /update with the .bin as the
// body and an X-Key header reflashes it over Wi-Fi, so a fix doesn't need
// BOOT+RST. The key is random per board (kept in NVS) and shown on screen: only
// someone who can see the board can reflash it. Images are not signed; this is
// a spike for a home network.

#include "stream.h"

#include <Arduino.h>
#include <Update.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFi.h>

static size_t ring_bytes = 6 * 1024 * 1024;  // about 35 s at 176.4 kB/s (4 MB if 6 won't fit)
constexpr uint32_t kRate = 44100;
constexpr uint32_t kBytesPerSec = kRate * 4;

static uint8_t *ring = nullptr;
static portMUX_TYPE ring_lock = portMUX_INITIALIZER_UNLOCKED;
static uint64_t written = 0;   // total bytes pushed, under ring_lock
static uint64_t take_end = 0;  // nonzero while a take runs: pushes stop here, under ring_lock

static char ssid[33], pass[65];
static char update_key[17];
static WiFiServer server(80);

struct StreamStats {
  uint32_t takes = 0, overruns = 0, max_stall_ms = 0, dropped_after_end = 0, take_ms = 0, take_secs = 0;
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
  const size_t at = written % ring_bytes;
  if (take_end && written + len > take_end) {
    const uint64_t room = take_end > written ? take_end - written : 0;
    if (room < len) ++stats.dropped_after_end;
    len = room;
  }
  taskEXIT_CRITICAL(&ring_lock);
  if (!len) return;
  const size_t first = min(len, ring_bytes - at);
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

static uint32_t content_length = 0;
static char request_key[17];

static bool key_matches() {
  uint8_t diff = strlen(request_key) != strlen(update_key);
  for (size_t i = 0; i < sizeof update_key; ++i) diff |= request_key[i] ^ update_key[i];
  return diff == 0 && update_key[0];
}

// Body: the raw app .bin (curl --data-binary). Writes the other OTA slot, then reboots.
static void update(WiFiClient &c) {
  if (!content_length || !Update.begin(content_length)) {
    c.printf("HTTP/1.0 400 Bad Request\r\n\r\nbegin failed: %s\n", Update.errorString());
    return;
  }
  static uint8_t buf[4096];
  uint32_t got = 0, last = millis();
  while (got < content_length && millis() - last < 10000) {
    const int n = c.read(buf, min<uint32_t>(sizeof buf, content_length - got));
    if (n <= 0) {
      delay(1);
      continue;
    }
    last = millis();
    if (Update.write(buf, n) != static_cast<size_t>(n)) break;
    got += n;
  }
  if (got == content_length && Update.end(true)) {
    c.print("HTTP/1.0 200 OK\r\n\r\nupdated, rebooting\n");
    c.flush();
    delay(300);
    ESP.restart();
  }
  Update.abort();
  c.printf("HTTP/1.0 500 Error\r\n\r\nupdate failed at %lu of %lu: %s\n", static_cast<unsigned long>(got),
           static_cast<unsigned long>(content_length), Update.errorString());
}

static void serve(WiFiClient &c) {
  // Request line, then headers; only Content-Length matters.
  String line = c.readStringUntil('\n');
  content_length = 0;
  memset(request_key, 0, sizeof request_key);
  while (c.connected()) {
    String h = c.readStringUntil('\n');
    if (h.length() <= 1) break;
    h.trim();
    if (h.startsWith("Content-Length:") || h.startsWith("content-length:")) content_length = h.substring(15).toInt();
    if (h.startsWith("X-Key:") || h.startsWith("x-key:")) {
      String k = h.substring(6);
      k.trim();
      strlcpy(request_key, k.c_str(), sizeof request_key);
    }
  }
  if (line.startsWith("POST /update")) {
    if (key_matches()) update(c);
    else c.print("HTTP/1.0 403 Forbidden\r\n\r\nX-Key header missing or wrong (it is on the board's screen)\n");
    return;
  }
  if (line.startsWith("GET /stats")) {
    char buf[256];
    stream_stats(buf, sizeof buf);
    c.printf("HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\n%s\nring %u bytes, max take %u s\n"
             "last take: %lu s of audio sent in %lu ms\n",
             buf, static_cast<unsigned>(ring_bytes), static_cast<unsigned>(ring_bytes / kBytesPerSec),
             static_cast<unsigned long>(stats.take_secs), static_cast<unsigned long>(stats.take_ms));
    return;
  }
  if (!line.startsWith("GET /take.wav")) {
    c.print("HTTP/1.0 404 Not Found\r\nContent-Type: text/plain\r\n\r\nGET /take.wav?s=30, GET /stats\n");
    return;
  }
  uint32_t secs = 30;
  const int q = line.indexOf("s=");
  if (q > 0) secs = constrain(line.substring(q + 2).toInt(), 1, 3600);
  secs = min<uint32_t>(secs, (ring_bytes - 64 * 1024) / kBytesPerSec);
  const uint32_t data_bytes = secs * kBytesPerSec;

  c.setNoDelay(true);
  c.printf("HTTP/1.0 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: %lu\r\n\r\n",
           static_cast<unsigned long>(44 + data_bytes));
  uint8_t h[44];
  wav_header(h, data_bytes);
  c.write(h, sizeof h);

  ++stats.takes;
  stats.client = true;
  stats.max_stall_ms = 0;
  stats.take_secs = secs;
  taskENTER_CRITICAL(&ring_lock);
  uint64_t pos = written & ~uint64_t{3};
  take_end = pos + data_bytes;
  taskEXIT_CRITICAL(&ring_lock);
  uint32_t left = data_bytes;
  const uint32_t started = millis();
  uint32_t last_progress = started;
  while (left && c.connected()) {
    uint64_t avail = written_now() - pos;
    if (avail > ring_bytes) ++stats.overruns;  // can't happen while take_end holds; counted if it does
    if (avail == 0) {
      delay(5);
      continue;
    }
    size_t n = min<uint64_t>(min<uint64_t>(avail, 16384), left);
    const size_t at = pos % ring_bytes;
    n = min(n, ring_bytes - at);
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
  stats.take_ms = millis() - started;
  taskENTER_CRITICAL(&ring_lock);
  take_end = 0;
  taskEXIT_CRITICAL(&ring_lock);
  stats.client = false;
}

static void stream_task(void *) {
  WiFi.persistent(false);
  WiFi.setHostname("fm1audio");
  WiFi.mode(WIFI_STA);
  WiFi.enableIPv6(true);  // link-local: mDNS answers the AAAA query macOS sends first
  WiFi.begin(ssid, pass);
  while (WiFi.status() != WL_CONNECTED) delay(200);
  // Modem sleep (the default) halves throughput here: 96 kB/s and 21-337 ms pings
  // against the 176 kB/s the audio needs.
  WiFi.setSleep(false);
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
  ring = static_cast<uint8_t *>(ps_malloc(ring_bytes));
  if (!ring) {
    ring_bytes = 4 * 1024 * 1024;
    ring = static_cast<uint8_t *>(ps_malloc(ring_bytes));
  }
  Preferences p;
  p.begin("wifi", true);
  strlcpy(ssid, p.getString("ssid", "").c_str(), sizeof ssid);
  strlcpy(pass, p.getString("pass", "").c_str(), sizeof pass);
  p.end();
  Preferences k;
  k.begin("spike", false);
  strlcpy(update_key, k.getString("key", "").c_str(), sizeof update_key);
  if (strlen(update_key) != 16) {
    for (int i = 0; i < 8; ++i) snprintf(update_key + 2 * i, 3, "%02x", static_cast<unsigned>(esp_random() & 0xFF));
    k.putString("key", update_key);
  }
  k.end();
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
    snprintf(out, len, "%s %s k:%s", WiFi.localIP().toString().c_str(), stats.client ? "TX" : "idle",
             update_key);
  }
}

void stream_stats(char *out, size_t len) {
  snprintf(out, len, "takes %lu %luk %lukbps ovr %lu st %lums", static_cast<unsigned long>(stats.takes),
           static_cast<unsigned long>(stats.sent / 1024), static_cast<unsigned long>(stats.last_kbps),
           static_cast<unsigned long>(stats.overruns), static_cast<unsigned long>(stats.max_stall_ms));
}
