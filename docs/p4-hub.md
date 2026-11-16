# Big-P4 hub firmware (spike, night 1)

`firmware/p4_hub` runs on Big-P4 (Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5, MAC
`e8:f6:0a:e8:a5:55`). It is the base the next tracks build on: the diag web page
(milestone E1) and the USB host behind the hub (C0-1). HW2 adds track D's graph
behind `hub_graph.h`.

## Build and flash

```
make p4-hub          # build into build/big_p4/p4-hub (cache in build/cache/p4-hub)
make flash-p4-hub    # MAC check over UART-C, then each piece at its own offset
```

`tools/p4_flash.sh` writes the bootloader (0x2000), the partition table (0x8000),
boot_app0 (0xe000) and the app (0x10000), never NVS (0x9000). Don't write a
merged image at 0x0 any more: its padding erases NVS, which holds the saved
Wi-Fi network. Opening UART-C resets the board, so after a flash, read the
board over Wi-Fi, not the console.

**Wi-Fi network:** NVS namespace `wifi` (`ssid`, `pass`), the same keys Outrigger
and the robots use. Big-P4's was seeded once on 2026-11-16 from Big-S3's backup
(only those two keys, repacked with `nvs_partition_gen`); the image lives outside
the repo. If NVS is ever erased, seed it again the same way.

## Reading it (never touching the board)

```
python3 -I tools/p4_diag.py all OUTDIR      # status, log, crash, graph, usb, screen.bmp
python3 -I tools/p4_diag.py log --since N
python3 -I tools/p4_diag.py post /bench/replug
```

`http://p4hub.local/` is the page itself (polls every 3 s). Routes are listed in
`firmware/p4_hub/hub_web.h`. macOS can take seconds to resolve `p4hub.local`;
`P4HUB_HOST=<ip>` skips mDNS.

- **/status:** build (git short SHA), uptime, boot count, reset reason, heap, Wi-Fi, USB summary, screen frame time.
- **/log?since=N:** numbered lines from our code (U USB, M main, N network) and ESP-IDF's own log (E),
  512 kept. The ESP-IDF hook also counts USB enumeration failures (`CHECK_SHORT_DEV_DESC`, `Port disabled`).
- **/crash:** this boot's reset reason, and the last abnormal reset's record: the panic (reason, core, PC,
  stack words), and the last 40 log lines before it. The record goes to NVS on the first boot after the
  crash, because every UART-C open is a power-on reset that would clear RTC memory. Plus a summary of the
  core dump in flash.
- **/coredump:** the stored core dump (a header, then the ELF). Decode:
  `esp_coredump --chip esp32p4 info_corefile -t raw -c coredump.bin build/big_p4/p4-hub/p4_hub.ino.elf`
  (with riscv32-esp-elf-gdb on PATH).
- **/graph:** the graph's state. A stub until HW2.
- **/screen.bmp:** 320x480 RGB565 BMP of the frame buffer last sent to the panel (the panel's MISO isn't
  wired, so it can't be read back).
- **/usb:** devices (address, hub port, VID:PID, product, serial, MIDI in/out cables, host channels),
  MIDI packets per cable, echo counts, and per identity (VID:PID:serial) connects, disconnects and the
  P4-side remount time (disconnect to next connect).

## USB host

The HS OTG port runs forced full speed (the probe's finding: no split transactions), with the large
periodic-OUT FIFO. 16 host channels. Every MIDI packet is echoed back to the device and cable it came
from (off by default; `POST /bench/echo?on=1` turns it on, and `tools/c0_1.py` does), so a robot checks both directions of both
cables by itself (`tools/robot_scenarios/c0-1-cables.json`). `tools/c0_1.py` runs C0-1.

**Known open bug (night 1, HW2): the USB host deadlocks on replug while MIDI is flowing.** Seen twice
in two hotplug-song runs (23 connects/disconnects in the second). The core dump
(`~/Projects/spike-ext/hw2logs/wedge/decoded.txt`) shows the EspUsbHost task in
`usbh_process -> handle_epn_halt_flush -> hcd_pipe_command(HALT) -> _internal_pipe_event_wait`,
waiting forever on a pipe of the departing device while holding the usbh mutex; the client task
(`usb_host_endpoint_clear`) and `graph_out` (`midiSend -> usb_host_transfer_submit`) wait on that
mutex forever. The P4 log shows `Enqueue URB error: ESP_ERR_INVALID_STATE` just before. Symptoms:
`/graph` `wire.tick_wakes` stops moving, `POST /bench/graph` returns `rc -100`. Recover with
`POST /bench/crash` (reboots, keeps a fresh core dump). Not diagnosed further.

## Board facts found here

- esp-hosted (Wi-Fi through the C6) needs a bigger loop stack: the default 8 KB panics with a stack
  protection fault in `WiFi.mode()`. `SET_LOOP_TASK_STACK_SIZE(32 * 1024)`.
- The C6's slave firmware predates the version RPC (`Req_GetCoprocessorFwVersion` times out; host 2.12.13).
  Wi-Fi works regardless.
- Screen: Waveshare's pins (MOSI 20, SCLK 21, CS 23, DC 26, RST 27, backlight 28). `Arduino_HWSPI` took
  2.49 s per full flush; `Arduino_ESP32SPI` (ESP-IDF's SPI master) 44 ms.
- Homebrew's ctags mangles some lines in an `.ino`; keep code in `.cpp` files.
