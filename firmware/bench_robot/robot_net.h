// The robot's Wi-Fi control line (ROBOT_WIFI builds, docs/bench-robot.md): the
// same line protocol as the UART, on TCP port 7000, plus ArduinoOTA on 3232 so
// a robot on the P4's hub is reflashed without a cable. It joins the network
// Outrigger saved in NVS namespace `wifi` (ssid, pass), read-only: flashing the
// robot leaves NVS alone, so a board that ran Outrigger already knows it.
//
// A task on core 0 owns the socket. The protocol task on core 1 only touches
// two stream buffers (one writer, one reader each), so a slow network never
// blocks a scheduled send.
#pragma once

#include <stddef.h>

namespace robot_net {

constexpr unsigned kPort = 7000;

void begin(const char *hostname);     // starts the network task
size_t read(char *dst, size_t n);     // control bytes in, without waiting
size_t write(const char *s, size_t n);  // queues output; dropped while no client
size_t tx_free();                     // room left in the output queue
bool has_client();
void describe(char *out, size_t n);   // "ip=... rssi=... host=..."

}  // namespace robot_net
