// The on-device UI, built on LVGL: the signal-chain screen and the settings
// page. Board-independent: layout comes from the BoardProfile, not from fixed
// pixel positions. Call everything from one task (the Arduino loop).
#pragma once

#include <stdint.h>

#include "board_profile.h"
#include "status.h"

struct UiSettings {
  uint8_t text_size;  // 0 small, 1 medium, 2 large
  uint8_t typeface;   // 0 sans (Montserrat), 1 pixel (Unscii)
};

// One device's line in the controller group.
struct UiDeviceLine {
  char tag[4];
  char detail[40];  // latest message with its mark, or "no USB channel"
  bool open;
  bool fresh;       // a message arrived in the last moment
};

// Everything the chain screen shows, gathered by the app each refresh.
struct UiFrame {
  midi_core::HostSnapshot snapshot;
  midi_core::ChainView view;
  UiDeviceLine controllers[midi_core::kMaxDevices];
  uint8_t controller_count;
  char hub_detail[48];
  char fm1_in[48];
  bool fm1_in_fresh;
  char fm1_own[48];
  uint32_t sent, blocked;
};

// What the app does when the user acts. Any hook may be null.
struct UiHooks {
  void (*settings_changed)(const UiSettings &);
  void (*connect_computer)();  // settings page: restart with the USB host off
  void (*back_to_host)();      // computer screen: restart as the MIDI host
};

// Sets up LVGL, the display and touch, and builds the screens.
// firmware: the running build's commit, shown on the settings page.
bool ui_begin(const BoardProfile &board, const UiSettings &settings, const UiHooks &hooks, const char *firmware);
// Shows the computer-mode screen instead of the chain (USB host not running).
// info: the running firmware's commit (so an update can be confirmed) and USB details.
void ui_show_computer_mode(const char *info);
// Refreshes the chain screen; cheap when nothing changed.
void ui_update(const UiFrame &frame);
// Runs LVGL; call often (every few ms).
void ui_tick();
