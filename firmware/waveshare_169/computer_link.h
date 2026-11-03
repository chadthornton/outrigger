// Computer mode's USB serial link (the chip's USB-Serial-JTAG): one task reads
// the port and hands every byte to each protocol on it, the board profile's
// ('O' 'P', lib/board/src/profile_link.h) and, in Loader builds, adding a
// firmware ('O' 'R', add_firmware.h). Each skips the other's frames.
#pragma once

// Starts the link; back_to_host restarts for normal use (the profile link's 'B').
void computer_link_begin(const char *build, void (*back_to_host)());
