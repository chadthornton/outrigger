// The USB host on Big-P4's high-speed OTG port (the hub's), forced to full
// speed as the probe found it must be (no split transactions). Keeps a table of
// what's connected, counts MIDI packets per device and cable, times each
// device's (re)mount, and can echo every MIDI packet back to the device and
// cable it came from (the robots' loopback check of both directions; off by
// default, C0-1's tools turn it on).
//
// The seam for HW2's graph: hub_usb_on_midi() registers the one MIDI-in
// callback (USB client task: copy and return, never block), and
// hub_usb_send() queues a USB-MIDI packet to a device from any task; the echo
// task drains that queue, so sends never run on the client task.
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
// Called on EspUsbHost's client task for every USB-MIDI packet (pkt[0] is
// cable << 4 | CIN). One callback; nullptr clears it.
typedef void (*HubMidiIn)(uint8_t address, const uint8_t pkt[4], uint32_t ms);
void hub_usb_on_midi(HubMidiIn fn);
// Queues one packet to a device's MIDI OUT. False when the queue is full.
bool hub_usb_send(uint8_t address, const uint8_t pkt[4]);
// Submits one packet to a device's MIDI OUT now, from the caller's task (HW2's
// graph out task; core 0). False if the device has no MIDI OUT or the submit failed.
bool hub_usb_send_now(uint8_t address, const uint8_t pkt[4]);
// HW2: called on EspUsbHost's client task (the same task as the MIDI callback)
// when a device connects or disconnects. serial is the one seen at connect.
typedef void (*HubDeviceFn)(uint8_t address, const char *serial, bool connected);
void hub_usb_on_device(HubDeviceFn fn);
void hub_usb_set_echo(bool on);
bool hub_usb_echo();
// Bench: a software replug of the whole bus (end, 1 s, begin), as the probe's `p`.
void hub_usb_request_replug();
// Bench: the external hub's port status, or power off/on. -1 when there's no hub.
int hub_usb_port_status(uint8_t port, uint16_t &status, uint16_t &change);
int hub_usb_port_power(uint8_t port, bool on);
// Bench: forget counters (not the device table).
void hub_usb_reset_counters();
