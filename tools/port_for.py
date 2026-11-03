"""Which USB port a board from the device registry (tools/devices.py) is on.

    python3 -I tools/port_for.py Small-V2        # prints /dev/cu.usbmodemXXXX, or exits 1

It reads the MAC of each ESP32 port the Mac sees, and every read ends with
esptool's hard reset, so no board, ours or another session's, is left in
download mode. A board in Computer mode comes back in Computer mode. The board
must show up as a port: Computer mode, or BOOT+RST download mode.
"""
import glob
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import devices  # noqa: E402


def esptool():
    found = sorted(glob.glob(str(Path.home() / "Library/Arduino15/packages/esp32/tools/esptool_py/*/esptool")))
    if not found:
        sys.exit("no esptool under ~/Library/Arduino15 (arduino-cli's ESP32 core)")
    return found[-1]


def read_mac(port, tries=3):
    """The MAC of the ESP32 on port, or None; the board is hard-reset after each try."""
    for _ in range(tries):
        out = subprocess.run([esptool(), "--chip", "auto", "--port", port, "--after", "hard-reset", "read-mac"],
                             capture_output=True, text=True, timeout=60).stdout
        m = re.search(r"MAC:\s+([0-9a-fA-F:]{17})", out)
        if m:
            return devices.norm_mac(m.group(1))
    return None


def port_for(label, reg=None, ports=None):
    reg = reg or devices.load()
    want = reg.find(label)
    if want["kind"] != "esp32":
        raise devices.DeviceError(f"{want['label']} is an {want['kind']}; port_for finds ESP32 boards")
    seen = []
    for port in sorted(ports if ports is not None else glob.glob("/dev/cu.usbmodem*")):
        mac = read_mac(port)
        if mac == want["mac"]:
            return port
        other = reg.by_mac(mac) if mac else None
        seen.append(f"{port}: {other['label'] if other else mac or 'no MAC read'}")
    raise devices.DeviceError(f"{want['label']} ({want['mac']}) isn't on any port"
                              + (f"; found {'; '.join(seen)}" if seen else ": no ESP32 ports at all")
                              + ". Is it in Computer mode, or BOOT+RST?")


def main(argv):
    if len(argv) != 1:
        sys.exit(__doc__)
    try:
        print(port_for(argv[0]))
    except devices.DeviceError as e:
        print(e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
