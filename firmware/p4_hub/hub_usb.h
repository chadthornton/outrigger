// The USB host on Big-P4's high-speed OTG port (the hub's), forced to full
// speed as the probe found it must be (no split transactions). Keeps a table of
// what's connected, counts MIDI packets per device and cable, times each
// device's (re)mount, and can echo every MIDI packet back to the device and
// cable it came from (the robots' loopback check of both directions).
//
// Threads: callbacks arrive on EspUsbHost's client task and only copy into a
// queue or bump counters under a spinlock; hub_usb_loop() (Arduino loop) owns
// the table's updates; the web task reads it through hub_usb_json().
#pragma once
#include <stddef.h>
#include <stdint.h>

bool hub_usb_begin();
void hub_usb_loop(uint32_t now_ms);
size_t hub_usb_json(char *out, size_t cap);
// One summary line per device for the screen.
int hub_usb_lines(char lines[][48], int max);
uint8_t hub_usb_device_count();
uint8_t hub_usb_channels_used();
uint8_t hub_usb_channels_max();
void hub_usb_set_echo(bool on);
bool hub_usb_echo();
// Bench: a software replug of the whole bus (end, 1 s, begin), as the probe's `p`.
void hub_usb_request_replug();
// Bench: the external hub's port status, or power off/on. -1 when there's no hub.
int hub_usb_port_status(uint8_t port, uint16_t &status, uint16_t &change);
int hub_usb_port_power(uint8_t port, bool on);
// Bench: forget counters (not the device table).
void hub_usb_reset_counters();
