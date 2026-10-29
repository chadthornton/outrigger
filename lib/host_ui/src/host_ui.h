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

// The Firmware page (the Loader's, in builds that carry it): what the loader
// is doing, a list of actions (back up, then each library package), the
// selected one's preview, and a 2-second hold that runs it.
constexpr int kLoaderLines = 6;
constexpr int kLoaderItems = 10;
struct UiLoaderItem {
  char label[28];  // "Skiff 1.4"
  char note[20];   // "running", "FM-1_900"...
  bool pinned;     // a favorite: drawn with a filled star
};
struct UiLoader {
  char title[40];                     // what the loader is doing, or its last result
  uint32_t title_colour;              // 0xRRGGBB
  char lines[kLoaderLines][40];       // store, blob, unit, hashes ...
  UiLoaderItem items[kLoaderItems];   // library packages, favorites first
  uint8_t item_count;
  int8_t selected;                    // -1: "Back up this FM-1"; else an item
  char preview[2][40];                // what the selected action would do
  char action[40];                    // the hold's label, e.g. "Hold: install SLOOP"
  char top_label[40];                 // the top row, if not "Back up this FM-1" (a retry)
  uint8_t percent;                    // progress while busy
  bool busy;                          // a job is running: the hold is off
  bool can_act;                       // the selected action can run now
};

// What the app does when the user acts. Any hook may be null.
struct UiHooks {
  void (*settings_changed)(const UiSettings &);
  void (*connect_computer)();  // settings page: restart with the USB host off
  void (*back_to_host)();      // computer screen: restart as the MIDI host
  // Firmware page. Null in builds without the Loader: no Firmware page then.
  void (*loader_select)(int index);  // a row was tapped (-1: back up)
  void (*loader_pin)(int index);     // a row's star was tapped
  void (*loader_act)();              // the 2-second hold completed
};

// Sets up LVGL, the display and touch, and builds the screens.
// firmware: the running build's commit, shown on the settings page.
bool ui_begin(const BoardProfile &board, const UiSettings &settings, const UiHooks &hooks, const char *firmware);
// Shows the computer-mode screen instead of the chain (USB host not running).
// info: the running firmware's commit, so an update can be confirmed.
void ui_show_computer_mode(const char *info);
// Opens the Firmware page (a loader job's boot shows its progress there).
void ui_show_firmware();
// Refreshes the chain screen; cheap when nothing changed.
void ui_update(const UiFrame &frame);
// Refreshes the Firmware page; cheap when nothing changed.
void ui_update_loader(const UiLoader &loader);
// The network line on the settings and computer-mode screens (the Web UI's
// address, or how to reach its setup hotspot). Empty hides it.
void ui_set_network(const char *line);
// Runs LVGL; call often (every few ms).
void ui_tick();
