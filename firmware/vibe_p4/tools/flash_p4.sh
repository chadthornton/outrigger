#!/bin/sh
# flash_p4.sh IMAGE.merged.bin: write one Vibe bench image to Big-P4 over UART-C, only if the MAC is Big-P4's.
# For HW3 (S1 never runs it). The image is one merged file written at 0x0, A1's recipe.
#   firmware/vibe_p4/tools/flash_p4.sh ~/Projects/spike-ext/s1/p4img/fast/vibe_bench_p4.merged.bin
set -eu
IMG="${1:?usage: flash_p4.sh IMAGE.merged.bin}"
PORT="${PORT:-/dev/cu.usbmodem5CF70432541}"
MAC="e8:f6:0a:e8:a5:55"
PY="$HOME/.espressif/python_env/idf5.5_py3.13_env/bin/python"   # esptool 4.13 (underscore spellings)
[ -f "$IMG" ] || { echo "no $IMG" >&2; exit 1; }
# every MAC read ends with a hard reset (the brief's rule)
out=$("$PY" -m esptool --chip esp32p4 --port "$PORT" --after hard_reset read_mac 2>&1) || { echo "$out" >&2; exit 1; }
echo "$out" | grep -qi "MAC: $MAC" || { echo "$out" >&2; echo "refusing: $PORT is not Big-P4 ($MAC)" >&2; exit 1; }
"$PY" -m esptool --chip esp32p4 --port "$PORT" -b 460800 --before default_reset --after hard_reset \
  write_flash 0x0 "$IMG"
