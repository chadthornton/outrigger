"""Log the probe's serial console to a file, unattended.

    uv run --with pyserial prototypes/p4_usb_probe/serial_log.py PORT LOG [CMD]

Every line gets a wall-clock timestamp. If CMD is given, the logger polls that
file; whatever is written to it is sent to the board (e.g. `t` to start the
tone, `s` to stop, `r` to report) and the file is emptied. Reconnects if the
port drops, so a board reset doesn't end the log.
"""

import os
import sys
import time

import serial


def main() -> None:
    port, log_path = sys.argv[1], sys.argv[2]
    cmd_path = sys.argv[3] if len(sys.argv) > 3 else None
    with open(log_path, "a", buffering=1) as log:
        while True:
            try:
                # dsrdtr/rtscts off and lines low: don't reset the board on open.
                ser = serial.Serial()
                ser.port, ser.baudrate, ser.timeout = port, 115200, 0.2
                ser.dtr = ser.rts = False
                ser.open()
            except serial.SerialException:
                time.sleep(1)
                continue
            log.write(f"{time.strftime('%H:%M:%S')} LOGGER open {port}\n")
            buf = b""
            try:
                while True:
                    buf += ser.read(4096)
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        text = line.decode(errors="replace").rstrip("\r")
                        log.write(f"{time.strftime('%H:%M:%S')} {text}\n")
                    if cmd_path and os.path.exists(cmd_path) and os.path.getsize(cmd_path):
                        with open(cmd_path, "r+") as f:
                            cmd = f.read()
                            f.truncate(0)
                        ser.write(cmd.encode())
                        log.write(f"{time.strftime('%H:%M:%S')} LOGGER sent {cmd.strip()!r}\n")
            except (serial.SerialException, OSError):
                log.write(f"{time.strftime('%H:%M:%S')} LOGGER lost {port}\n")
                time.sleep(1)


if __name__ == "__main__":
    main()
