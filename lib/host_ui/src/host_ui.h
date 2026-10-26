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
  uint8_t wifi;       // 0 off, 1 on (web control panel)
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
  char net_line[48];   // settings page: address, setup hotspot, or "off"
  char net_line2[48];
  char banner1[24];    // shown over any screen while set, e.g. "Pair browser"
  char banner2[24];    // e.g. the PIN
};

using UiSettingsChanged = void (*)(const UiSettings &);

// Sets up LVGL, the display and touch, and builds both screens.
bool ui_begin(const BoardProfile &board, const UiSettings &settings, UiSettingsChanged on_change);
// Refreshes the chain screen; cheap when nothing changed.
void ui_update(const UiFrame &frame);
// Runs LVGL; call often (every few ms).
void ui_tick();
// Applies settings changed elsewhere (the web page); doesn't call on_change.
void ui_set_settings(const UiSettings &settings);
