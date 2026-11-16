#!/bin/sh
# Build fm1-p4-emu for the ESP32-P4 (rev 3.x) and lay out the images to flash in out/:
#   out/bootloader.bin       at 0x2000
#   out/partition-table.bin  at 0x8000   (from esp/partitions.csv)
#   out/fm1-p4-emu.bin       at 0x10000
# and, with packages named, out/<name>.pkg.bin for the fwpkg partition at 0x410000:
#   firmware/p4_emu/build.sh [PKG.fwsc ...]
# Needs rustup's nightly (esp/rust-toolchain.toml), ldproxy, and Python 3.13 for ESP-IDF's
# tools (pyenv 3.13.3); ESP-IDF v5.5.5 and its tools land in ~/.espressif on the first build.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
export PATH="$HOME/.cargo/bin:$PATH" PYENV_VERSION="${PYENV_VERSION:-3.13.3}"
(cd "$here/esp" && cargo build --release)
rel="$here/esp/target/riscv32imafc-esp-espidf/release"
idf_out=$(dirname "$(ls -t "$rel"/build/esp-idf-sys/*/out/build/flasher_args.json | head -1)")
idf="$HOME/.espressif/esp-idf/v5.5.5"
py="$HOME/.espressif/python_env/idf5.5_py3.13_env/bin/python"
min=$(sed -n 's/^CONFIG_ESP_REV_MIN_FULL=//p' "$idf_out/../sdkconfig")
max=$(sed -n 's/^CONFIG_ESP_REV_MAX_FULL=//p' "$idf_out/../sdkconfig")
mkdir -p "$here/out"
cp "$idf_out/bootloader/bootloader.bin" "$here/out/bootloader.bin"
"$py" "$idf/components/partition_table/gen_esp32part.py" --flash-size 8MB \
    "$here/esp/partitions.csv" "$here/out/partition-table.bin"
"$py" -m esptool --chip esp32p4 elf2image --flash_mode dio --flash_freq 80m --flash_size 8MB \
    --elf-sha256-offset 0xb0 --min-rev-full "$min" --max-rev-full "$max" \
    -o "$here/out/fm1-p4-emu.bin" "$rel/fm1-p4-emu"
for pkg in "$@"; do
    name=$(basename "$pkg" .fwsc)
    python3 -I "$here/tools/mkpkg.py" "$pkg" "$here/out/$name.pkg.bin" "$name"
done
ls -l "$here/out"
