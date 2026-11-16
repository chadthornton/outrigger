#!/bin/sh
# Flash an Arduino build onto Big-P4 over UART-C without touching NVS (0x9000):
# bootloader 0x2000, partition table 0x8000, boot_app0 0xe000, app 0x10000.
# Never the merged image at 0x0: its padding erases NVS, which holds the saved
# Wi-Fi network. Reads the MAC first (ending in a hard reset) and refuses any
# board but Big-P4.
#   tools/p4_flash.sh BUILD_DIR SKETCH_NAME [NVS_BIN]
# NVS_BIN, when given, is written to 0x9000 (the one-time Wi-Fi seed).
set -eu
BUILD=$1; NAME=$2; NVS=${3:-}
PORT=${P4_PORT:-/dev/cu.usbmodem5CF70432541}
MAC=e8:f6:0a:e8:a5:55
PY=${P4_PY:-$HOME/.espressif/python_env/idf5.5_py3.13_env/bin/python}
BOOT_APP0=$HOME/Library/Arduino15/packages/esp32/hardware/esp32/3.3.12/tools/partitions/boot_app0.bin
out=$("$PY" -m esptool --chip esp32p4 --port "$PORT" --after hard_reset read_mac 2>&1) || { echo "$out"; echo "can't read the MAC on $PORT; not flashing"; exit 1; }
echo "$out" | grep -q "MAC: $MAC" || { echo "$out"; echo "$PORT is not Big-P4 ($MAC); not flashing"; exit 1; }
echo "Big-P4 ($MAC) on $PORT"
set -- 0x2000 "$BUILD/$NAME.ino.bootloader.bin" 0x8000 "$BUILD/$NAME.ino.partitions.bin" 0xe000 "$BOOT_APP0" 0x10000 "$BUILD/$NAME.ino.bin"
[ -n "$NVS" ] && set -- "$@" 0x9000 "$NVS"
"$PY" -m esptool --chip esp32p4 --port "$PORT" -b 460800 --before default_reset --after hard_reset write_flash "$@"
