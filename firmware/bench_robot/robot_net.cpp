// See robot_net.h. Compiled only into ROBOT_WIFI builds.
#if ROBOT_WIFI
#include "robot_net.h"

#include <Arduino.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFi.h>

#include "freertos/stream_buffer.h"

namespace robot_net {
namespace {

// The UART build's buffers were 4 KB in and 16 KB out; the same here.
constexpr size_t kRxCap = 4096, kTxCap = 16384;
StreamBufferHandle_t rx, tx;
char host[24];
char ip_str[16] = "-";
volatile int rssi = 0;
volatile bool client_on = false;
volatile bool ota_busy = false;

void net_task(void *) {
  char ssid[33] = "", pass[65] = "";
  {
    Preferences p;
    if (p.begin("wifi", true)) {  // read-only: never rewrites Outrigger's record
      strlcpy(ssid, p.getString("ssid", "").c_str(), sizeof ssid);
      strlcpy(pass, p.getString("pass", "").c_str(), sizeof pass);
      p.end();
    }
  }
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(host);
  WiFi.setSleep(false);  // power save adds 100 ms wake-ups to every reply
  WiFi.setAutoReconnect(true);
  if (ssid[0]) WiFi.begin(ssid, pass);
  memset(pass, 0, sizeof pass);

  WiFiServer server(kPort);
  WiFiClient c;
  bool started = false;
  static char buf[1460];
  for (;;) {
    bool busy = false;
    bool up = WiFi.status() == WL_CONNECTED;
    if (up && !started) {
      ArduinoOTA.setHostname(host);  // also starts mDNS as <host>.local
      ArduinoOTA.onStart([] { ota_busy = true; });
      ArduinoOTA.onEnd([] { ota_busy = false; });
      ArduinoOTA.onError([](ota_error_t) { ota_busy = false; });
      ArduinoOTA.begin();
      MDNS.addService("robot", "tcp", kPort);
      server.begin();
      server.setNoDelay(true);
      started = true;
    }
    if (up) {
      strlcpy(ip_str, WiFi.localIP().toString().c_str(), sizeof ip_str);
      rssi = WiFi.RSSI();
    } else {
      strlcpy(ip_str, "-", sizeof ip_str);
    }
    if (started) {
      ArduinoOTA.handle();
      if (server.hasClient()) {  // the newest connection wins
        WiFiClient n = server.accept();
        if (c) c.stop();
        c = n;
        c.setNoDelay(true);
      }
    }
    client_on = c && c.connected();
    if (client_on) {
      size_t room = xStreamBufferSpacesAvailable(rx);  // leave the rest in TCP: backpressure
      int avail = c.available();
      if (avail > 0 && room > 0) {
        int k = c.read((uint8_t *)buf, min((size_t)avail, min(room, sizeof buf)));
        if (k > 0) xStreamBufferSend(rx, buf, k, 0);
        busy = true;
      }
      size_t k = xStreamBufferReceive(tx, buf, sizeof buf, 0);
      if (k) {
        c.write((const uint8_t *)buf, k);
        busy = true;
      }
    } else {
      // Nobody listening: drop output, so a drain never waits on a full queue.
      while (xStreamBufferReceive(tx, buf, sizeof buf, 0)) {
      }
    }
    if (!busy) vTaskDelay(1);
  }
}

}  // namespace

void begin(const char *hostname) {
  strlcpy(host, hostname, sizeof host);
  rx = xStreamBufferCreate(kRxCap, 1);
  tx = xStreamBufferCreate(kTxCap, 1);
  xTaskCreatePinnedToCore(net_task, "robot-net", 8192, nullptr, 5, nullptr, 0);
}

size_t read(char *dst, size_t n) { return xStreamBufferReceive(rx, dst, n, 0); }

size_t write(const char *s, size_t n) { return xStreamBufferSend(tx, s, n, 0); }

size_t tx_free() { return xStreamBufferSpacesAvailable(tx); }

bool has_client() { return client_on; }

void describe(char *out, size_t n) {
  snprintf(out, n, "ip=%s rssi=%d host=%s.local%s", ip_str, rssi, host, ota_busy ? " ota=busy" : "");
}

}  // namespace robot_net
#endif
