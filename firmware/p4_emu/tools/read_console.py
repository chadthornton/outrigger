#!/usr/bin/env python3
"""Read fm1-p4-emu's console for a while and summarise its speed reports.

    ~/.espressif/python_env/idf5.5_py3.13_env/bin/python -I tools/read_console.py PORT SECONDS [LOG]

Opens PORT (the P4's USB Serial/JTAG) without toggling DTR/RTS, so the board keeps running, prints
every line, appends them to LOG if given, and ends with the mean of the FM1EMU reports' `ratio`
and `emu_mcyc_s` (the first report is left out: it includes the boot). Needs pyserial (in ESP-IDF's
Python env).
"""
import sys
import time

import serial


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    port, secs = sys.argv[1], float(sys.argv[2])
    log = open(sys.argv[3], "a") if len(sys.argv) > 3 else None
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.5
    s.dtr = s.rts = False
    s.open()
    end, buf, reports = time.time() + secs, b"", []
    while time.time() < end:
        buf += s.read(4096)
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            text = line.decode("utf-8", "replace").rstrip("\r")
            print(text, flush=True)
            if log:
                log.write(text + "\n")
            if text.startswith("FM1EMU fw="):
                reports.append(dict(kv.split("=", 1) for kv in text.split()[1:] if "=" in kv))
    s.close()
    steady = reports[1:] or reports
    if steady:
        r = sum(float(x["ratio"]) for x in steady) / len(steady)
        c = sum(float(x["emu_mcyc_s"]) for x in steady) / len(steady)
        print("SUMMARY fw=%s reports=%d mean_ratio=%.4f mean_emu_mcyc_s=%.2f last_emu_ms=%s free_psram=%s"
              % (steady[-1]["fw"], len(steady), r, c, steady[-1]["emu_ms"], steady[-1]["free_psram"]))
    else:
        print("SUMMARY no FM1EMU reports in %.0f s" % secs)


if __name__ == "__main__":
    main()
