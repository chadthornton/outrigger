#include <Arduino.h>
// Big-P4 Wi-Fi positive control (spike night 1, HW1): bring up the ESP32-C6
// co-processor through esp-hosted (SDIO, the core's default pins), scan, and
// join the network saved in NVS namespace `wifi` if there is one. Prints the
// SSID count and the join result only, never the network's password.
#include <WiFi.h>
#include <Preferences.h>

SET_LOOP_TASK_STACK_SIZE(32 * 1024)

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("WIFISCAN begin");
  Serial.flush();
  WiFi.mode(WIFI_STA);
  Serial.println("WIFISCAN mode ok");
  Serial.flush();
  uint32_t t0 = millis();
  int n = WiFi.scanNetworks();
  Serial.printf("WIFISCAN networks=%d ms=%lu\n", n, (unsigned long)(millis() - t0));
  for (int i = 0; i < n && i < 8; i++) Serial.printf("WIFISCAN rssi=%d ch=%d\n", WiFi.RSSI(i), WiFi.channel(i));
  Preferences p;
  char ssid[33] = "", pass[65] = "";
  if (p.begin("wifi", true)) {
    strlcpy(ssid, p.getString("ssid", "").c_str(), sizeof ssid);
    strlcpy(pass, p.getString("pass", "").c_str(), sizeof pass);
    p.end();
  }
  Serial.printf("WIFISCAN saved=%d\n", ssid[0] ? 1 : 0);
  if (!ssid[0]) return;
  t0 = millis();
  WiFi.begin(ssid, pass);
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(100);
  Serial.printf("WIFISCAN joined=%d ms=%lu ip=%s rssi=%d mac=%s\n", WiFi.status() == WL_CONNECTED,
                (unsigned long)(millis() - t0), WiFi.localIP().toString().c_str(), WiFi.RSSI(),
                WiFi.macAddress().c_str());
}

void loop() { delay(1000); }
