#!/bin/sh
# Cross-compiles lib/graph/src for ESP32-P4 (RISC-V) and ESP32-S3 (Xtensa) with
# the Arduino core's own toolchains and IDF flags, -Os, and prints each
# object's text/data/bss. Builds only; nothing is linked or flashed.
#   tools/graph_esp_sizes.sh [~/Library/Arduino15/packages/esp32/tools]
set -eu
TOOLS=${1:-$HOME/Library/Arduino15/packages/esp32/tools}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$ROOT/build/graph-esp
LIBVER=$(ls "$TOOLS/esp32p4-libs" | sort | tail -1)
for target in esp32p4 esp32s3; do
  case $target in
    esp32p4) CC=$(ls -d "$TOOLS"/esp-rv32/*/bin | sort | tail -1)/riscv32-esp-elf-gcc ;;
    esp32s3) CC=$(ls -d "$TOOLS"/esp-x32/*/bin | sort | tail -1)/xtensa-esp32s3-elf-gcc ;;
  esac
  LIBS=$TOOLS/${target}-libs/$LIBVER
  mkdir -p "$OUT/$target"
  objs=""
  for src in "$ROOT"/lib/graph/src/*.c; do
    o=$OUT/$target/$(basename "$src" .c).o
    # gcc reads the IDF flag files itself (@file keeps their quoting).
    "$CC" @"$LIBS/flags/c_flags" @"$LIBS/flags/defines" \
      -iprefix "$LIBS/include/" @"$LIBS/flags/includes" -I"$LIBS/dio_qspi/include" \
      -Os -std=gnu11 -Wall -Wextra -Werror -c "$src" -o "$o"
    objs="$objs $o"
  done
  # The Schwung shim and both modules, built from source the same way.
  for src in "$ROOT"/lib/graph/shim/gr_schwung.c "$ROOT"/lib/graph/third_party/eucalypso/eucalypso.c \
      "$ROOT"/lib/graph/third_party/superarp/superarp.c; do
    name=$(basename "$src" .c)
    o=$OUT/$target/$name.o
    "$CC" @"$LIBS/flags/c_flags" @"$LIBS/flags/defines" \
      -iprefix "$LIBS/include/" @"$LIBS/flags/includes" -I"$LIBS/dio_qspi/include" \
      -Os -std=gnu11 -w -I"$ROOT/lib/graph/src" -I"$ROOT/lib/graph/shim" \
      -I"$ROOT/lib/graph/third_party/schwung" -Dmove_midi_fx_init="${name}_midi_fx_init" -c "$src" -o "$o"
    objs="$objs $o"
  done
  SIZE=$(dirname "$CC")/$(basename "$CC" gcc)size
  echo "== $target ($($CC --version | head -1))"
  # shellcheck disable=SC2086
  "$SIZE" -t $objs | sed "s|$OUT/$target/||"
done
