// Compiles one board's generated board_config.h (tools/boards.py) on the Mac,
// built once per profile by `make test`, so a profile that generates bad C++
// fails here rather than in an Arduino build.
#include <cstdio>

#include "board_config.h"
#include "board_profile.h"

static const BoardProfile kProfile = BOARD_PROFILE_INIT;

int main() {
  static_assert(kBoardConfig.rotation < 4, "Arduino_GFX's rotations are 0-3");
  const int native_w = kBoardConfig.rotation & 1 ? kProfile.height : kProfile.width;
  if (native_w <= 0 || kBoardConfig.spi_hz == 0) return 1;
  printf("board_config: %s (tag %d), %dx%d, panel %d, touch %d\n", kProfile.name, BOARD_TAG_ID, kProfile.width,
         kProfile.height, int(kBoardConfig.panel), int(kBoardConfig.touch));
  return 0;
}
