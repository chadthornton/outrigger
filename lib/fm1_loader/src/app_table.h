// An FM-1 image's own file table: the encrypted records at 0x4000 that tell its
// bootloader where the app, the JieLi VM settings store and the Bluetooth data
// (BTIF) live. Read-only. It is how the loader tells the two flash layouts apart:
// a firmware's identity or USB ID can't (FM-1_8xxxxxx firmwares come in both).
//
// - V15 layout: stock V15 and everything built on it (Felucca, Skiff, SLOOP ...).
//   App 0x4000-0x92FFF, VM 0x93000-0xE8FFF, BTIF 0xE9000 (layout.h).
// - Open layout: M-VAVE's FM1 Open (Oct 2026, github.com/m-vave/fm1_open) and
//   firmwares built on its SDK. VM starts where the image ends (it varies by
//   build), BTIF at 0x7B000, then a fixed user partition 0x7C000-0xFEFFF
//   (sdk/Package.cs). Its user data overlaps the V15 app area, stock's VM and
//   factory data, 0xE9000 and stock's resources: docs/open-firmware.md.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "layout.h"

namespace fm1_loader {

constexpr uint32_t kOpenBtif = 0x7B000;
constexpr uint32_t kOpenUserStart = 0x7C000;  // 0xFF000 - 536576 (sdk/Package.cs UserSize)

struct AppTable {
  uint32_t image_end = 0;  // first sector past the app: app_area_head's span, rounded up
  Range vm{0, 0};
  Range btif{0, 0};
};

// Reads the table from flash bytes that start at address 0: a package's
// flash.bin or a whole-flash backup. False unless app_area_head, VM and BTIF
// records are present with good CRCs and sane, sector-aligned addresses.
bool read_app_table(const uint8_t *flash, size_t len, AppTable &out);

enum class Layout { Unknown, V15, Open };
// Matches the table against the two known layouts exactly; anything else is Unknown.
Layout layout_of(const AppTable &t);
const char *layout_name(Layout l);

// Where an Open-layout firmware keeps its data, from M-VAVE's app/instrument.h
// and app/project.c (V14). End addresses exclusive.
struct OpenMap {
  Range vm;        // JieLi VM: MIDI and system preferences
  Range btif;      // Bluetooth pairing data
  Range projects;  // 15 project slots
  Range settings;  // one sector
  Range journal;   // project staging and the tuning journal
  Range samples;   // sample bank, header first; a write erases only the sectors it fills
  Range library;   // DX7 voice library, four banks
  Range tuning;    // per-track tuning
};
OpenMap open_map(const AppTable &t);

}  // namespace fm1_loader
