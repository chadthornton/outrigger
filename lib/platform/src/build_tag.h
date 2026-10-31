// The build tag: which build this is, where the web installer can read it off
// a board without running it. The linker puts .rodata_custom_desc right after
// esp_app_desc_t (sections.ld), so the tag sits at app image offset 0x120,
// flash 0x10120. esp_app_desc_t's own version field is arduino-lib-builder's,
// the same in every build, so it can't name ours.
//
// Readers: site/installer/board.js (readBuildTag) and tools/build_site.py
// (read_build_tag, which refuses to stage a build without one). Change the
// layout only with both, and bump kBuildTagLayout; tests/installer/tags.json
// holds the cases.
#pragma once

#include <stdint.h>

#include "apps.h"

namespace platform {

constexpr uint32_t kBuildTagMagic = 0x54484D55;  // "UMHT"
constexpr uint16_t kBuildTagLayout = 1;

struct BuildTag {
  uint32_t magic;
  uint16_t layout;
  uint8_t dirty;      // 1: built with uncommitted changes
  uint8_t reserved;
  uint32_t commit;    // FW_VERSION: the short commit as a number
  char app_set[8];    // APP_SET_LABEL, NUL-padded
};
static_assert(sizeof(BuildTag) == 20, "the web installer reads 20 bytes");

}  // namespace platform
