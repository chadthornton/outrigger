#include "computer_link.h"

#include <Arduino.h>
#include <driver/usb_serial_jtag.h>

#include <vector>

#include "apps.h"
#include "board_store.h"
#include "profile_link.h"
#if APP_LOADER
#include "add_firmware.h"
#endif

namespace {

// The driver's buffers: a whole add-firmware 'D' frame fits in the receive one.
constexpr uint32_t kRxBuffer = 8192, kTxBuffer = 1024;

void (*restart_for_host)() = nullptr;

void link_task(void *arg) {
  ProfileLink &profile = *static_cast<ProfileLink *>(arg);
  static uint8_t in[512];
  std::vector<uint8_t> out;
  for (;;) {
    const int n = usb_serial_jtag_read_bytes(in, sizeof in, pdMS_TO_TICKS(200));
    out.clear();
    if (n > 0) profile.feed(in, size_t(n), out);
#if APP_LOADER
    add_firmware_feed(in, n > 0 ? size_t(n) : 0, out);
#endif
    for (size_t at = 0; at < out.size();) {
      const int w = usb_serial_jtag_write_bytes(out.data() + at, out.size() - at, pdMS_TO_TICKS(1000));
      if (w <= 0) break;  // the computer isn't reading: the page resends
      at += size_t(w);
    }
    if (profile.restart_due() && restart_for_host) {
      delay(200);  // the 'b' reply leaves before the port drops
      restart_for_host();
    }
  }
}

}  // namespace

bool computer_link_begin(const char *build, void (*back_to_host)()) {
  restart_for_host = back_to_host;
#if APP_LOADER
  add_firmware_begin(build);
#else
  (void)build;
#endif
  usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
  cfg.rx_buffer_size = kRxBuffer;
  cfg.tx_buffer_size = kTxBuffer;
  if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) return false;
  auto *profile = new ProfileLink({board_store_load, board_store_save, board_store_forget, board_build_quad_psram()});
  xTaskCreatePinnedToCore(link_task, "computer_link", 8192, profile, 2, nullptr, 1);
  return true;
}
