// Wi-Fi WAV stream for the audio spike (stream.cpp).
#pragma once

#include <stddef.h>
#include <stdint.h>

// Allocates the PSRAM ring and starts Wi-Fi with the saved network. False when
// there is no PSRAM or no saved network; stream_status() says which.
bool stream_begin();
// Called from the USB audio callback with each packet's payload.
void stream_push(const uint8_t *data, size_t len);
// One line each for the screen.
void stream_status(char *out, size_t len);
void stream_stats(char *out, size_t len);
