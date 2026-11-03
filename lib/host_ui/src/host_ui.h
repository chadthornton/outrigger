// The on-device UI, built on LVGL (docs/ui-redesign.md): three sections under
// a nav (Play, FM-1 in Loader builds, Setup), the sub-pages under them, the
// job pages and Computer mode. Board-independent: layout comes from the
// BoardProfile's screen shape, not from fixed pixel positions. Call everything
// from one task (the Arduino loop).
#pragma once

#include <stdint.h>

#include "board_profile.h"
#include "status.h"

struct UiSettings {
  uint8_t text_size;  // 0 small, 1 medium, 2 large
};

// One device's line in the controller group.
struct UiDeviceLine {
  char tag[4];
  char detail[40];  // latest message with its mark, or "no USB channel"
  bool open;
  bool fresh;       // a message arrived in the last moment
};

// Everything Play shows, gathered by the app each refresh.
struct UiFrame {
  midi_core::HostSnapshot snapshot;
  midi_core::ChainView view;
  midi_core::StageStatus stages[midi_core::kStages];  // each line's status (stage_status)
  UiDeviceLine controllers[midi_core::kMaxDevices];
  uint8_t controller_count;
  char hub_detail[48];
  char fm1_in[48];
  bool fm1_in_fresh;
  char fm1_own[48];
  uint32_t sent, blocked;
};

// ---------- The FM-1 section (Loader builds) ----------
// Split by page: the firmware list, the plan before a switch, a running job,
// and the result after one. Plain fixed-size data: loader_app.cpp copies it
// under a spinlock, where nothing may allocate.
constexpr int kLoaderItems = 10;
constexpr int kLoaderLines = 6;

// What a 2-second hold runs. The Loader refuses a hold whose kind (and, for a
// switch, firmware) isn't what it last previewed.
enum class UiAct : uint8_t { None, Switch, Backup, Retry };

enum class UiNote : uint8_t { None, Running, YourSongs, NotYet };
struct UiLoaderItem {
  char label[28];  // "Skiff 1.4"
  UiNote note;     // running: what the FM-1 runs; your songs: a save of this unit
};

// What holding "switch to X" would do.
constexpr int kPlanLines = 6;
struct UiPlan {
  int8_t item;                   // the list row it's for; -1 before the Loader has one
  char title[28];                // "Felucca 1.6.1"
  char lines[kPlanLines][48];    // "Switches from Skiff 1.4." ...
  uint8_t line_count;
  uint8_t warn_from;             // lines from here on are warnings (orange); line_count when none
  char hold[40];                 // the hold's label after "Hold: ", "switch to Felucca 1.6.1"
  bool can_act;
};

// A running job: a title, then its steps, then a bar.
enum class UiStepState : uint8_t { Done, Now, Next };
constexpr int kJobSteps = 5;
struct UiJob {
  bool active;     // a job is running (or the board is restarting for one)
  char title[28];  // "Switching to Felucca 1.6.1"
  struct {
    UiStepState state;
    char text[40];
  } steps[kJobSteps];
  uint8_t step_count;
  uint8_t percent;
  bool danger;     // Installer::danger(): the red "Don't unplug" card shows
  char note[40];   // under the bar: "Writes nothing to the FM-1."
};

// A result or an error: the headline, whether the FM-1 is safe, why, what to
// do, the detail for us, and the hold that runs the fix (or OK).
enum class UiSafety : uint8_t { NothingWritten, WrittenChecked, Danger };
struct UiResult {
  uint32_t seq;          // a new result: the UI opens its page once
  bool ok;               // the job finished: a green headline
  UiSafety safety;
  char headline[28];     // "Switched", "Switch didn't start"
  char safety_text[48];  // "Nothing was written."
  char why[72];
  char todo[64];
  char detail[96];
  UiAct fix;             // None: an OK button
  char fix_label[40];    // after "Hold: ", "retry Felucca 1.6.1"
};

struct UiLoader {
  UiLoaderItem items[kLoaderItems];  // the library, the running one first
  uint8_t item_count;
  // Headline set: the FM-1 page shows this instead of the list (the store or
  // the loader file is bad, or an install is unfinished).
  UiResult blocked;
  bool unfinished;          // an install stopped part-way: red dot on FM-1, and Play says so
  char running[28];         // what the FM-1 runs, for Play ("Skiff 1.4"), or ""
  char backups_note[40];    // the Backups row: "Latest b50"
  bool can_back_up, can_retry;
  UiPlan plan;
  UiJob job;
  UiResult result;
  // For the Web UI's /status only, not drawn: the job's state and today's
  // diagnostic lines (store, blob, unit, hashes ...).
  char title[40];
  uint32_t title_colour;  // 0xRRGGBB
  uint8_t percent;
  char lines[kLoaderLines][40];
};

// What the app does when the user acts. Any hook may be null.
struct UiHooks {
  void (*settings_changed)(const UiSettings &);
  void (*connect_computer)();  // the Computer mode hold: restart with the USB host off
  void (*back_to_host)();      // Computer mode screen: restart as the MIDI host
  // The FM-1 section. Null in builds without the Loader: no FM-1 section then.
  void (*loader_select)(int index);  // a firmware's plan was opened
  // A 2-second hold completed: what its label named, and for a switch the row.
  void (*loader_act)(UiAct act, int index);
};

// Sets up LVGL, the display and the input, and builds the screens.
// firmware: the running build's commit, shown on Setup.
bool ui_begin(const BoardProfile &board, const UiSettings &settings, const UiHooks &hooks, const char *firmware);
// Shows the Computer mode screen (USB host not running).
// info: the running firmware's commit, so an update can be confirmed.
void ui_show_computer_mode(const char *info);
// The first screen of a boot a firmware job restarted into: the board restarts
// around every job (loader_app.cpp, JobBoot), and this keeps the screen on the
// job instead of flashing Play. Drawn at once; the job's or result's page, or
// Play, replaces it at the next ui_update_loader.
void ui_show_switching(const char *title);
// Refreshes Play; cheap when nothing changed.
void ui_update(const UiFrame &frame);
// Refreshes the FM-1 section, and opens a job's page or a new result's.
void ui_update_loader(const UiLoader &loader);
// The network line on Setup and Computer mode (the Web UI's address, or how to
// reach its setup hotspot). Empty hides it.
void ui_set_network(const char *line);
// The setup hotspot as a Wi-Fi QR code's text (WIFI:T:WPA;S:...;;), drawn under
// Setup's network line so a phone joins with one scan. Empty removes it.
void ui_set_join_qr(const char *text);
// The Computer mode screen's line about a firmware being added from the
// browser ("Adding Felucca 1.8.0: 40%"). Empty hides it.
void ui_set_computer_status(const char *line);
// Runs LVGL; call often (every few ms).
void ui_tick();
// For ui-shots: the fonts a label may use at the current text size, one per
// type role, then the icon font and Setup's S/M/L letters. Returns how many.
int ui_scale_fonts(const void *fonts[], int max);
