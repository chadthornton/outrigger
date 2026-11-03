#!/bin/zsh
# Waits for a board to appear on the Mac and prints its port, after checking
# its MAC. For hardware cards: run it, then the person picks Computer mode (or
# presses BOOT+RST) on the board.
#
#   tools/wait_for_board.sh Small-V2 [timeout-seconds] [--stay]
#
# The board is named by its label in the device registry (tools/devices.py), or
# by a MAC. Safe beside other sessions' boards:
# - ports already there when it starts are never touched;
# - every MAC read ends with esptool's hard reset, so no board is left in
#   download mode (one in Computer mode comes back in Computer mode);
# - --stay leaves the matching board in its bootloader instead, for a board
#   brought up with BOOT+RST whose own program would take the port away.
# Exit 0 with the port on stdout; 1 on timeout. Messages go to stderr.
setopt nullglob
want=${1:l}; timeout=${2:-3600}; stay=${3:-}
[[ -z $want ]] && { echo "usage: $0 <label|mac> [timeout-seconds] [--stay]" >&2; exit 2; }
if [[ $want != *:*:*:*:*:* ]]; then
  want=$(python3 -I "${0:A:h}/devices.py" mac "$1") || exit 2
fi
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
      [[ $mac == $want && $stay == --stay ]] || $esptool --port $p --before no-reset --after hard-reset read-mac >/dev/null 2>&1
      [[ -n $mac ]] && break
    done
    if [[ $mac == $want ]]; then
      echo "board $want on $p" >&2
      echo $p
      exit 0
    fi
    echo "$p is ${mac:-not an ESP32 we could read}, not $want: reset and left alone" >&2
  done
  sleep 1
done
echo "no board $want after ${timeout}s" >&2
exit 1
