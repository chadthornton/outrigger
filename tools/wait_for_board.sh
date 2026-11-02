#!/bin/zsh
# Waits for a board to appear on the Mac and prints its port, after checking
# its MAC. For hardware cards: run it, then the person presses BOOT+RST (or
# picks Computer mode) on the board.
#
#   tools/wait_for_board.sh 44:bd:8d:61:2b:28 [timeout-seconds]
#
# Safe beside other sessions' boards:
# - ports already there when it starts are never touched;
# - a new port whose MAC doesn't match gets esptool's hard reset, so a probe
#   never leaves someone else's board stuck in download mode;
# - the matching board is left in its bootloader (--after no-reset), ready
#   for the next esptool or arduino-cli command.
# Exit 0 with the port on stdout; 1 on timeout. Messages go to stderr.
setopt nullglob
want=${1:l}; timeout=${2:-3600}
[[ -z $want ]] && { echo "usage: $0 <mac> [timeout-seconds]" >&2; exit 2; }
esptool=$(ls -d ~/Library/Arduino15/packages/esp32/tools/esptool_py/*/esptool(N) | tail -1)
[[ -z $esptool ]] && { echo "no esptool under ~/Library/Arduino15" >&2; exit 2; }
before=(/dev/cu.usbmodem*)
checked=()
for (( i = 0; i < timeout; i++ )); do
  for p in /dev/cu.usbmodem*; do
    (( ${before[(Ie)$p]} || ${checked[(Ie)$p]} )) && continue
    checked+=$p
    # A board just plugged in or restarting can miss the first try or two.
    mac=""
    for try in 1 2 3; do
      sleep 2
      mac=$($esptool --chip auto --port $p --after no-reset read-mac 2>&1 | grep -m1 'MAC:' | awk '{print tolower($2)}')
      [[ -n $mac ]] && break
    done
    if [[ $mac == $want ]]; then
      echo "board $want on $p" >&2
      echo $p
      exit 0
    fi
    echo "$p is ${mac:-not an ESP32 we could read}, not $want: resetting it and leaving it alone" >&2
    $esptool --port $p --before no-reset --after hard-reset read-mac >/dev/null 2>&1
  done
  sleep 1
done
echo "no board $want after ${timeout}s" >&2
exit 1
