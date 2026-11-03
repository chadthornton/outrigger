"""Read, set or forget the board profile stored on an Outrigger, over Computer
mode's USB serial link (lib/board/src/profile_link.h). The board must be in
Computer mode (Setup > hold Computer mode).

    build/tools-venv/bin/python tools/board_profile.py <port> get
    build/tools-venv/bin/python tools/board_profile.py <port> set <board id>   # a profile from firmware/boards.json
    build/tools-venv/bin/python tools/board_profile.py <port> forget
    build/tools-venv/bin/python tools/board_profile.py <port> restart          # leave Computer mode

A stored profile applies at the next start. The port opens with DTR and RTS
off: on the ESP32-S3's own USB port, either one held reset or download mode.
"""
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import boards  # noqa: E402

PSRAM_MISMATCH, STORE_FAILED, BAD_REQUEST = 100, 101, 102


def frame(kind, payload=b""):
    body = bytes([kind]) + len(payload).to_bytes(2, "little") + payload
    return b"OP" + body + boards.crc16_xmodem(body).to_bytes(2, "little")


def read_frame(port, timeout=5.0):
    """The next 'O' 'P' frame's (type, payload); other bytes (the board's log) are skipped."""
    buf = bytearray()
    end = time.time() + timeout
    while time.time() < end:
        buf += port.read(256)
        while len(buf) >= 2 and buf[:2] != b"OP":
            del buf[0]
        if len(buf) >= 5:
            n = buf[3] | buf[4] << 8
            if len(buf) >= 7 + n:
                body = bytes(buf[2:5 + n])
                if boards.crc16_xmodem(body) == int.from_bytes(buf[5 + n:7 + n], "little"):
                    return chr(body[0]), body[3:]
                del buf[0]
    raise TimeoutError("no reply from the board: is it in Computer mode?")


def ask(port, kind, payload=b""):
    port.write(frame(ord(kind), payload))
    kind, payload = read_frame(port)
    if kind == "e":
        raise RuntimeError(f"the board refused (code {payload[0]}): {payload[1:].decode(errors='replace')}")
    return kind, payload


def open_port(name):
    import serial
    p = serial.Serial()
    p.port, p.baudrate, p.timeout = name, 115200, 0.2
    p.dtr = p.rts = False  # either one held resets the S3, or keeps it in download mode
    p.open()
    return p


def describe(blob_bytes):
    b = boards.unblob(blob_bytes)
    s, d = b["screen"], b["display"]
    return f"{b['name']} (tag {b['tag_id']}): {s['width']}x{s['height']}, {d['driver']}, rotation {d['rotation']}"


def main(argv):
    if len(argv) < 2 or argv[1] not in ("get", "set", "forget", "restart"):
        sys.exit(__doc__)
    port_name, action = argv[0], argv[1]
    table = boards.checked(boards.load())
    with open_port(port_name) as port:
        if action == "get":
            _, payload = ask(port, "G")
            print(describe(payload) if payload else "no profile stored: the board runs its build's own")
        elif action == "set":
            if len(argv) < 3 or argv[2] not in table["boards"]:
                sys.exit(f"set needs a board id: {' '.join(boards.all_ids(table))}")
            data = boards.blob(table["boards"][argv[2]])
            ask(port, "S", data)
            print(f"stored {describe(data)}; it applies at the next start")
        elif action == "forget":
            ask(port, "F")
            print("forgotten: the next start uses the build's own profile")
        else:
            ask(port, "B")
            print("restarting for normal use")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
