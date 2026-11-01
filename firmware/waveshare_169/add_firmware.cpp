#include "apps.h"
#if APP_LOADER

#include "add_firmware.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <driver/usb_serial_jtag.h>

#include <string>

#include "host_ui.h"
#include "library.h"
#include "posix_storage.h"
#include "receive.h"

using namespace fm1_loader;

static_assert(kMaxPackages == size_t(kLoaderItems), "an added firmware the Firmware page can't show");

namespace {

// As loader_app.cpp mounts it: never format, it holds backups.
constexpr const char *kStoreMount = "/store";
// The driver's buffers: a whole 'D' frame fits in the receive one.
constexpr uint32_t kRxBuffer = 8192, kTxBuffer = 1024;

portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
char line[64] = "";  // under lock

void set_line(const ReceiveStatus &s) {
  char text[sizeof line];
  switch (s.state) {
    case ReceiveStatus::State::Receiving:
      snprintf(text, sizeof text, "Adding %s: %u%%", s.label.c_str(),
               unsigned(s.total ? uint64_t(s.got) * 100 / s.total : 0));
      break;
    case ReceiveStatus::State::Adding:
      snprintf(text, sizeof text, "Storing %s...", s.label.c_str());
      break;
    case ReceiveStatus::State::Added:
      snprintf(text, sizeof text, "Added %s", s.label.c_str());
      break;
    case ReceiveStatus::State::Failed:
      snprintf(text, sizeof text, "Not added: %s", s.why.c_str());
      break;
    default:
      text[0] = 0;
  }
  taskENTER_CRITICAL(&lock);
  memcpy(line, text, sizeof line);
  taskEXIT_CRITICAL(&lock);
}

void receive_task(void *arg) {
  Receiver &rx = *static_cast<Receiver *>(arg);
  static uint8_t in[512];
  std::vector<uint8_t> out;
  for (;;) {
    const int n = usb_serial_jtag_read_bytes(in, sizeof in, pdMS_TO_TICKS(200));
    out.clear();
    if (n > 0) rx.feed(in, size_t(n), millis(), out);
    rx.tick(millis());
    for (size_t at = 0; at < out.size();) {
      const int w = usb_serial_jtag_write_bytes(out.data() + at, out.size() - at, pdMS_TO_TICKS(1000));
      if (w <= 0) break;  // the computer isn't reading: the page resends
      at += size_t(w);
    }
  }
}

}  // namespace

void add_firmware_begin(const char *build) {
  if (!LittleFS.begin(false, kStoreMount, 5, "store")) {
    strlcpy(line, "Store unreadable: firmwares can't be added", sizeof line);
    return;
  }
  usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
  cfg.rx_buffer_size = kRxBuffer;
  cfg.tx_buffer_size = kTxBuffer;
  if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) {
    strlcpy(line, "USB serial didn't start: firmwares can't be added", sizeof line);
    return;
  }
  // Lives as long as Computer mode: until the board restarts.
  auto *storage = new PosixStorage(kStoreMount);
  auto *library = new Library(*storage);
  auto *rx = new Receiver(*library, [] { return uint64_t(LittleFS.totalBytes() - LittleFS.usedBytes()); }, build);
  rx->on_status(set_line);
  xTaskCreatePinnedToCore(receive_task, "add_fw", 8192, rx, 2, nullptr, 1);
}

void add_firmware_line(char *out, size_t len) {
  char copy[sizeof line];
  taskENTER_CRITICAL(&lock);
  memcpy(copy, line, sizeof line);
  taskEXIT_CRITICAL(&lock);
  strlcpy(out, copy, len);
}

#endif  // APP_LOADER
