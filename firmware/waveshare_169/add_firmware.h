// Adding a firmware from the browser, in Computer mode: the page on
// outrigger.music sends a Firmware Hub package over the board's own USB port
// (USB-Serial-JTAG), and fm1_loader's Receiver stores it. Built only with
// APP_LOADER; Computer mode runs no loader task, so this owns the store.
//
// Threads: the receiver runs on its own task (an add hashes and writes for a
// few seconds); add_firmware_line() runs on the Arduino loop, for LVGL.
#pragma once

#include <stddef.h>

// Mounts the store and starts listening. Not in crash safe mode.
void add_firmware_begin(const char *build);
// The screen's line about an add, or "" when there's nothing to say.
void add_firmware_line(char *out, size_t len);
