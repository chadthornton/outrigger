#!/usr/bin/env python3
"""Read fm1-p4-emu's console for a while and summarise its speed reports.

    ~/.espressif/python_env/idf5.5_py3.13_env/bin/python -I tools/read_console.py [--reset] [--send TEXT] PORT SECONDS [LOG]

Opens PORT (Big-P4's UART-C port, the CH343P at /dev/cu.usbmodem5CF70432541) without toggling DTR/RTS,
so the board keeps running. With --reset it then pulses RTS (EN low, IO0 left high: a normal boot,
the same pulse as esptool's hard reset) so the boot log is captured from its first line. With
--send it writes TEXT once, a second after opening (a command for A1's bench). With
--from-emu-ms N (first) the SUMMARY's window starts at the first report at or past N emulated ms
(past the boot); the window ratio is (last emu_ms - first emu_ms) / (last wall_s - first wall_s).
A line starting "ESP-ROM" after the first boot is counted as a reboot. It prints
every line, appends them to LOG if given, and ends with the mean of the FM1EMU reports' `ratio`
and `emu_mcyc_s` (the first report is left out: it includes the boot). Needs pyserial (in ESP-IDF's
Python env).
"""
import sys
import time

import serial


def main():
    args, reset, send, from_ms = sys.argv[1:], False, None, 0
    if args[:1] == ["--from-emu-ms"]:
        from_ms, args = int(args[1]), args[2:]
    if args[:1] == ["--reset"]:
        reset, args = True, args[1:]
    if args[:1] == ["--send"]:
        send, args = args[1].encode(), args[2:]
    if len(args) < 2:
        sys.exit(__doc__)
    port, secs = args[0], float(args[1])
    log = open(args[2], "a") if len(args) > 2 else None
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.5
    s.dtr = s.rts = False
    s.open()
    if reset:
        s.rts = True
        time.sleep(0.1)
        s.rts = False
    end, buf, reports, boots = time.time() + secs, b"", [], 0
    sent_at = time.time() + 1.0
    while time.time() < end:
        if send is not None and time.time() >= sent_at:
            s.write(send)
            send = None
        buf += s.read(4096)
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            text = line.decode("utf-8", "replace").rstrip("\r")
            print(text, flush=True)
            if log:
                log.write(text + "\n")
            if text.startswith("ESP-ROM"):
                boots += 1
            if text.startswith("FM1EMU fw="):
                reports.append(dict(kv.split("=", 1) for kv in text.split()[1:] if "=" in kv))
    s.close()
    boots = max(0, boots - 1) if reset else boots  # the first boot is the one --reset asked for
    if not reports:
        print("SUMMARY no FM1EMU reports in %.0f s boots=%d" % (secs, boots))
        return
    win = [x for x in reports if int(x["emu_ms"]) >= from_ms] if from_ms else reports[1:] or reports
    minp = min(int(x["free_psram"]) for x in reports)
    if len(win) < 2:
        print("SUMMARY fw=%s window_reports=%d (too few past emu_ms %d) last_emu_ms=%s last_wall_s=%s min_free_psram=%d boots=%d"
              % (reports[-1]["fw"], len(win), from_ms, reports[-1]["emu_ms"], reports[-1]["wall_s"], minp, boots))
        return
    a, b = win[0], win[-1]
    dms = int(b["emu_ms"]) - int(a["emu_ms"])
    dwall = float(b["wall_s"]) - float(a["wall_s"])
    mean = sum(float(x["ratio"]) for x in win[1:]) / len(win[1:])
    print("SUMMARY fw=%s window emu_ms %s..%s wall_s %s..%s (%.1f s) window_ratio=%.5f mean_report_ratio=%.5f "
          "reports=%d cpu1=%s min_free_psram=%d last_free_int=%s boots=%d"
          % (b["fw"], a["emu_ms"], b["emu_ms"], a["wall_s"], b["wall_s"], dwall, dms / 1000.0 / dwall, mean,
             len(win), b["cpu1"], minp, b["free_int"], boots))

if __name__ == "__main__":
    main()
