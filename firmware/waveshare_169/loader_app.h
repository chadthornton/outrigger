// The Loader app on the Waveshare: the on-device store (LittleFS on the `store`
// partition), JieLi's loader blob, and a loader task that runs fm1_loader's
// Installer against the FM-1 on the hub. Built only with APP_LOADER.
//
// Session 2 scope (docs/firmware-loader-plan.md): "Back up this FM-1" only.
// The build is read-only (FM1_LOADER_WRITES off), so nothing it does can write
// the FM-1's flash.
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

// owns_fm1_midi: asks the broker whether the Loader owns the FM-1's MIDI face
// right now; the ROM-boot key is sent only then.
void loader_begin(EspUsbHost &usb, bool (*owns_fm1_midi)());
void loader_device_connected(const EspUsbHostDeviceInfo &d);
void loader_device_disconnected(const EspUsbHostDeviceInfo &d);
void loader_midi(const EspUsbHostMidiMessage &m);  // messages from the FM-1

void loader_request_backup();  // the Firmware page's hold
// The mode the USB host should run in now: Load while a job needs the FM-1,
// Play otherwise. loop() restarts the host when it differs, then reports back.
platform::Mode loader_wanted_mode();
void loader_mode_changed(platform::Mode mode);
void loader_ui(UiLoader &out);
