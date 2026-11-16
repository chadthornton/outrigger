"""Send one command to the P4 bench and log its console until a line matches.

    uv run --with pyserial firmware/synth_bench_p4/p4_console.py PORT LOG CMD UNTIL [TIMEOUT_S]

CMD is sent as-is ('' sends nothing). Every line is printed and appended to
LOG with a wall-clock timestamp. Exits 0 when a line contains UNTIL, 1 on
timeout. Opens the port with DTR and RTS low, so opening doesn't reset it.
"""

import sys
import time

import serial


def main() -> int:
    port, log_path, cmd, until = sys.argv[1:5]
    timeout = float(sys.argv[5]) if len(sys.argv) > 5 else 60
    ser = serial.Serial()
    ser.port, ser.baudrate, ser.timeout = port, 115200, 0.2
    ser.dtr = ser.rts = False
    ser.open()
    end = time.time() + timeout
    with open(log_path, "a", buffering=1) as log:
        log.write(f"{time.strftime('%H:%M:%S')} CONSOLE sent {cmd!r}, until {until!r}\n")
        if cmd:
            ser.write(cmd.encode())
        buf = b""
        while time.time() < end:
            buf += ser.read(4096)
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode(errors="replace").rstrip("\r")
                log.write(f"{time.strftime('%H:%M:%S')} {text}\n")
                print(text, flush=True)
                if until and until in text:
                    return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
