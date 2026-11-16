// The bench robot on an ESP32-S3 DevKitC-1 (docs/bench-robot.md): the native
// USB port is a two-cable USB MIDI device for the P4's hub, the UART port
// (Serial, through the board's USB-UART bridge) is the Mac's control line.
// The protocol and the schedule are robot_core.cpp; this file is the board.
#include <Arduino.h>

#include "USB.h"
#include "esp32-hal-tinyusb.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "robot_core.h"

#if ARDUINO_USB_MODE
#error "bench_robot needs USBMode=default (USB-OTG, TinyUSB) on the native port: make bench-robot sets it"
#endif
#if ARDUINO_USB_CDC_ON_BOOT
#error "bench_robot needs CDCOnBoot=default: Serial is the UART control port"
#endif

#ifndef ROBOT_FW
#define ROBOT_FW 0
#endif

namespace {

constexpr uint32_t kBaud = 921600;
// Internal RAM only (no PSRAM build option needed): about 176 KB. robot.py
// drains the log while a long run goes, so these are buffers, not run limits.
constexpr size_t kSchedCap = 2048, kTxCap = 4096, kRxCap = 4096;
robot::Event sched[kSchedCap];
robot::Record tx_log[kTxCap], rx_log[kRxCap];

char mac_str[18];   // aa:bb:cc:dd:ee:ff
char serial_str[13];  // the USB serial number: the MAC's hex, so two robots on one hub differ

class UsbPort : public robot::Port {
 public:
  bool send(const uint8_t pkt[4]) override { return connected_ && tud_midi_packet_write(pkt); }
  bool mounted() override { return connected_ && tud_mounted(); }
  void set_connected(bool on) override {
    connected_ = on;
    if (on) tud_connect();
    else tud_disconnect();
  }
  void describe(char *out, size_t n) override {
    snprintf(out, n, "mac=%s fw=%07x", mac_str, (unsigned)ROBOT_FW);
  }

 private:
  volatile bool connected_ = true;
};

UsbPort port;
robot::Core core(port, sched, kSchedCap, tx_log, kTxCap, rx_log, kRxCap);

class SerialOut : public robot::Out {
 public:
  void line(const char *s) override {
    Serial.write(s);
    Serial.write('\n');
  }
  // A record line is under 32 bytes; leave room for a status line too.
  bool can_write() override { return Serial.availableForWrite() > 400; }
};
SerialOut out;

// Two cables, each with an embedded and an external IN and OUT jack
// (TinyUSB's TUD_MIDI_DESCRIPTOR is the one-cable case of this).
constexpr uint16_t kMidiDescLen = TUD_MIDI_DESC_HEAD_LEN + 2 * TUD_MIDI_DESC_JACK_LEN + 2 * TUD_MIDI_DESC_EP_LEN(2);

uint16_t load_midi_descriptor(uint8_t *dst, uint8_t *itf) {
  uint8_t str = tinyusb_add_string_descriptor("Bench Robot");
  uint8_t str1 = tinyusb_add_string_descriptor("Robot cable 1");
  uint8_t str2 = tinyusb_add_string_descriptor("Robot cable 2");
  uint8_t ep_in = tinyusb_get_free_in_endpoint();
  uint8_t ep_out = tinyusb_get_free_out_endpoint();
  if (!ep_in || !ep_out) return 0;
  uint8_t d[kMidiDescLen] = {
      TUD_MIDI_DESC_HEAD(*itf, str, 2),
      TUD_MIDI_DESC_JACK_DESC(1, str1),
      TUD_MIDI_DESC_JACK_DESC(2, str2),
      TUD_MIDI_DESC_EP(ep_out, CFG_TUD_ENDPOINT_SIZE, 2),
      TUD_MIDI_JACKID_IN_EMB(1),
      TUD_MIDI_JACKID_IN_EMB(2),
      TUD_MIDI_DESC_EP((uint8_t)(0x80 | ep_in), CFG_TUD_ENDPOINT_SIZE, 2),
      TUD_MIDI_JACKID_OUT_EMB(1),
      TUD_MIDI_JACKID_OUT_EMB(2),
  };
  *itf += 2;  // the Audio Control interface and the MIDI Streaming one
  memcpy(dst, d, sizeof d);
  return sizeof d;
}

void on_usb_event(void *, esp_event_base_t base, int32_t id, void *) {
  if (base != ARDUINO_USB_EVENTS) return;
  if (id == ARDUINO_USB_STARTED_EVENT) core.mount_changed(true, esp_timer_get_time());
  else if (id == ARDUINO_USB_STOPPED_EVENT) core.mount_changed(false, esp_timer_get_time());
}

// The protocol task: UART in, sends on time, the log out. It sleeps a tick
// when nothing is due for 2 ms, and spins (yielding) closer than that, so a
// send leaves within a few microseconds of its time. TinyUSB's task runs at
// the top priority, so received packets are stamped even while this spins.
void protocol_task(void *) {
  for (;;) {
    int64_t now = esp_timer_get_time();
    while (Serial.available()) core.feed((char)Serial.read(), now, out);
    core.poll(esp_timer_get_time(), out);
    int64_t due = core.next_due();
    now = esp_timer_get_time();
    // A drain waiting on a full UART buffer sleeps too: spinning through it
    // would starve this core's idle task for seconds.
    bool soon = due == INT64_MIN ? out.can_write() : due - now <= 2000;
    if (soon || Serial.available()) taskYIELD();
    else vTaskDelay(1);
  }
}

}  // namespace

// TinyUSB calls this from its task when packets arrive; one stamp per USB
// transfer, taken before reading them out.
extern "C" void tud_midi_rx_cb(uint8_t itf) {
  int64_t t = esp_timer_get_time();
  uint8_t pkt[4];
  while (tud_midi_n_packet_read(itf, pkt)) core.received(pkt, t);
}

void setup() {
  Serial.setRxBufferSize(4096);
  Serial.setTxBufferSize(16384);
  Serial.begin(kBaud);
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(mac_str, sizeof mac_str, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  snprintf(serial_str, sizeof serial_str, "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  tinyusb_enable_interface(USB_INTERFACE_MIDI, kMidiDescLen, load_midi_descriptor);
  USB.onEvent(on_usb_event);
  USB.productName("Bench Robot");
  USB.manufacturerName("Outrigger bench");
  USB.serialNumber(serial_str);
  USB.begin();
  Serial.printf("# bench_robot fw=%07x mac=%s baud=%u\n", (unsigned)ROBOT_FW, mac_str, (unsigned)kBaud);
  xTaskCreatePinnedToCore(protocol_task, "robot", 8192, nullptr, configMAX_PRIORITIES - 2, nullptr, 1);
}

void loop() { vTaskDelay(1000); }
