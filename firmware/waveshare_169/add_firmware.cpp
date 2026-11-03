#include "apps.h"
#if APP_LOADER

#include "add_firmware.h"

#include <Arduino.h>
#include <LittleFS.h>

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

Receiver *receiver = nullptr;

}  // namespace

void add_firmware_begin(const char *build) {
  if (!LittleFS.begin(false, kStoreMount, 5, "store")) {
    strlcpy(line, "Store unreadable: firmwares can't be added", sizeof line);
    return;
  }
  // Lives as long as Computer mode: until the board restarts. computer_link.cpp
  // owns the USB serial port and hands it every byte (add_firmware_feed).
  auto *storage = new PosixStorage(kStoreMount);
  auto *library = new Library(*storage);
  receiver = new Receiver(*library, [] { return uint64_t(LittleFS.totalBytes() - LittleFS.usedBytes()); }, build);
  receiver->on_status(set_line);
}

void add_firmware_feed(const uint8_t *in, size_t len, std::vector<uint8_t> &out) {
  if (!receiver) return;
  if (len) receiver->feed(in, len, millis(), out);
  receiver->tick(millis());
}

void add_firmware_line(char *out, size_t len) {
  char copy[sizeof line];
  taskENTER_CRITICAL(&lock);
  memcpy(copy, line, sizeof line);
  taskEXIT_CRITICAL(&lock);
  strlcpy(out, copy, len);
}

#endif  // APP_LOADER
