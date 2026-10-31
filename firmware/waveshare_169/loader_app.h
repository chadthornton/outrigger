// The Loader app on the Waveshare: the on-device store (LittleFS on the `store`
// partition), JieLi's loader blob, and a loader task that runs fm1_loader's
// Installer against the FM-1 on the hub. Built only with APP_LOADER.
//
// It backs up the FM-1, installs library packages (only in builds with
// FM1_LOADER_WRITES=1; others are read-only), and retries an install that
// stopped part-way (the resume entry).
//
// Threads: the *_connected/_disconnected/_midi hooks run on EspUsbHost's client
// task; loader_request_backup(), loader_wanted_mode(), loader_mode_changed() and
// loader_ui() on the Arduino loop; the Installer on its own task, because
// EspUsbHost refuses mass-storage commands from its client task and LVGL may
// only run in loop().
#pragma once

#include <EspUsbHost.h>

#include "broker.h"
#include "host_ui.h"

// What the loader needs from the core.
struct LoaderCore {
  // Whether the broker gives the Loader the FM-1's MIDI face right now; the
  // ROM-boot key is sent only then.
  bool (*owns_fm1_midi)();
  // The broker's screen line for a device it refused (empty when claimed).
  void (*refusal)(uint8_t address, char *out, size_t len);
};
void loader_begin(EspUsbHost &usb, const LoaderCore &core);
void loader_device_connected(const EspUsbHostDeviceInfo &d);
void loader_device_disconnected(const EspUsbHostDeviceInfo &d);
void loader_midi(const EspUsbHostMidiMessage &m);  // messages from the FM-1

// The Firmware page: select a row (-1 = back up), pin a package, run the
// selected action after the 2-second hold.
void loader_select(int index);
void loader_pin(int index);
void loader_act();
// The mode this boot's USB host runs in, fixed until the next restart: Load
// when loader_begin() found a job recorded before the restart, Play otherwise.
// A job is its own boot (loader_app.cpp, JobBoot): the host never restarts in place.
platform::Mode loader_boot_mode();
// A job is running (a Load boot until it ends): nothing may restart the board.
bool loader_busy();
#if LOADER_SOAK
void loader_soak_crashed();  // safe mode, before the soak's restart
#endif
void loader_ui(UiLoader &out);
