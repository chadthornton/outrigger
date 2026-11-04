// The text of a Wi-Fi QR code: a phone's camera reads it and offers to join
// the network, so Chad scans the setup hotspot instead of typing its password.
#pragma once

#include <stddef.h>

namespace web_core {

// WIFI:T:WPA;S:<ssid>;P:<pass>;; into out, with \ ; , : " in the name and
// password escaped by a backslash, as the format asks. False, and out empty,
// when it doesn't fit in cap bytes (with the terminating 0).
bool wifi_qr_payload(const char *ssid, const char *pass, char *out, size_t cap);

}  // namespace web_core
