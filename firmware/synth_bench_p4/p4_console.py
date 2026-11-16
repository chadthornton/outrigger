"""Run commands on the P4 bench over Big-P4's UART-C port and log its console.

    ~/.espressif/python_env/idf5.5_py3.13_env/bin/python -I firmware/synth_bench_p4/p4_console.py \\
        PORT LOG [CMD UNTIL TIMEOUT_S]...

PORT is Big-P4's UART-C port (the CH343P, /dev/cu.usbmodem5CF70432541). Opening
it on macOS resets the board (night 0), so the script resets it on purpose too
(an RTS pulse: EN low, IO0 high, a normal boot), waits for the boot's
"BENCH ready" line, and only then runs the steps in order: each sends CMD
('' sends nothing) and waits up to TIMEOUT_S for a line containing UNTIL.
Every line is printed and appended to LOG with a wall-clock timestamp.

Exits 0 when every step matched, 1 when a step timed out, 2 when "BENCH ready"
never came, and 3 when not even the ROM's boot banner ("ESP-ROM") came: the
P4 is likely powered off (the CH343P stays on USB regardless), and a person
must press POWER.
"""

import sys
import time

import serial


def main() -> int:
    if len(sys.argv) < 3 or (len(sys.argv) - 3) % 3:
        sys.exit(__doc__)
    port, log_path = sys.argv[1:3]
    rest = sys.argv[3:]
    steps = [(rest[i], rest[i + 1], float(rest[i + 2])) for i in range(0, len(rest), 3)]
    ser = serial.Serial()
    ser.port, ser.baudrate, ser.timeout = port, 115200, 0.2
    ser.dtr = ser.rts = False
    ser.open()
    ser.rts = True
    time.sleep(0.1)
    ser.rts = False
    buf = b""
    seen_rom = False

    def stamp(log, text):
        log.write(f"{time.strftime('%H:%M:%S')} {text}\n")

    def wait_for(log, until, timeout):
        nonlocal buf, seen_rom
        end = time.time() + timeout
        while time.time() < end:
            buf += ser.read(4096)
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode(errors="replace").rstrip("\r")
                stamp(log, text)
                print(text, flush=True)
                if "ESP-ROM" in text:
                    seen_rom = True
                if until and until in text:
                    return True
        return False

    with open(log_path, "a", buffering=1) as log:
        stamp(log, f"CONSOLE opened {port}, reset, waiting for 'BENCH ready'")
        if not wait_for(log, "BENCH ready", 15):
            why = "no BENCH ready" if seen_rom else "no ESP-ROM boot banner: P4 likely off, press POWER"
            stamp(log, f"CONSOLE {why}")
            print(f"CONSOLE {why}", flush=True)
            return 2 if seen_rom else 3
        for cmd, until, timeout in steps:
            stamp(log, f"CONSOLE sent {cmd!r}, until {until!r}, timeout {timeout:.0f} s")
            if cmd:
                ser.write(cmd.encode())
            if not wait_for(log, until, timeout):
                stamp(log, f"CONSOLE timeout waiting for {until!r}")
                print(f"CONSOLE timeout waiting for {until!r}", flush=True)
                return 1
        stamp(log, "CONSOLE all steps matched")
    return 0


if __name__ == "__main__":
    sys.exit(main())
