// Which board profile runs, and the one stored on the board (board.cpp).
//
// At the start, board_select() takes the stored profile (set over Computer
// mode, profile_link.h) if it decodes, passes the checks and matches the
// build's PSRAM mode; else the build's own (board_config.h). A stored profile
// that crashed the board three starts in a row is set aside, and BOOT held for
// 3 s in the first 15 s after a start forgets it.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "board_profile.h"

// Before board_display_begin; returns where the profile came from, in words.
const char *board_select();
const BoardProfile &board_profile();
const char *board_source();
bool board_build_quad_psram();
// Watches BOOT for the first 15 s on its own task; held 3 s, it forgets the
// stored profile and calls on_escape. Call right after board_select().
void board_watch_escape(void (*on_escape)());

size_t board_store_load(uint8_t *out, size_t max);
bool board_store_save(const uint8_t *data, size_t len);
void board_store_forget();
