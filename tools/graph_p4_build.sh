#!/bin/sh
# Builds firmware/graph_hub_p4 (lib/graph's hub on Big-P4) with arduino-cli,
# same FQBN as HW1's Big-P4 images. Compile only: flashing is HW2's, through
# the MAC-checked path on spike/platform-p4-hw.
#   tools/graph_p4_build.sh [extra -D flags, e.g. -DGRAPH_HUB_APP=GR_HUB_APP_THRU]
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
FQBN=esp32:esp32:esp32p4:ChipVariant=postv3,PSRAM=enabled,USBMode=hwcdc,CDCOnBoot=default,PartitionScheme=huge_app
OUT=$ROOT/build/big_p4/graph-hub-p4
arduino-cli compile --fqbn "$FQBN" --build-property "tools.ctags.path=${CTAGS_DIR:-/opt/homebrew/bin}" \
  --build-property "compiler.cpp.extra_flags=${*:-}" --build-property "compiler.c.extra_flags=${*:-}" \
  --library "$ROOT/lib/graph" --build-path "$ROOT/build/cache/big_p4/graph-hub-p4" --output-dir "$OUT" \
  "$ROOT/firmware/graph_hub_p4"
ls -l "$OUT"/*.bin
