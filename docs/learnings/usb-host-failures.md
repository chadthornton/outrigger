# What broke in the USB host, and why

[Index](index.md) · previous: [USB MIDI host on the ESP32-S3](usb-host-esp32s3.md) · next: [The FM-1 on USB and in ROM boot](fm1-usb-and-rom-boot.md)

Every failure here happened on the board, and most of them only on the board. The Mac tests didn't catch any of
them, because they live in timing, task stacks and ESP-IDF's host driver. Ordered by when they were found.

## First, a way to see crashes

In host mode the board's only USB port is the host, so there's no serial log. Early crashes showed only as a
board that rebooted every few seconds. The project built its own crash forensics, and every later diagnosis below
depends on them:

| Tool | What it keeps | Commit |
|---|---|---|
| Breadcrumbs | Each task (loop, loader, USB) writes the step it's in to RTC memory, which survives a panic or watchdog reset. | `231af19` |
| Safe mode | After an abnormal reset the board starts without the USB host, shows the reason and the last steps, and hands the port to the Mac. A crash loop can't repeat, and the board can be reflashed without BOOT+RST. | `231af19` |
| Core dumps | ESP-IDF writes them to the `coredump` partition; `esp-coredump` decodes them on the Mac. | partition since session 2 |
| Web diagnostics | `/status`, `/log`, `/crash`, `/coredump`, `/store` over Wi-Fi, read by `tools/diag.py`. Safe mode starts Wi-Fi too. | `2d5ae67` |
| RTC log tail | The last 40 log lines of 96 bytes survive every reset except power-on, so `/crash` shows the lines before a panic. | `3242075`, `71b8efb` |
| Soak build | `make build ... SOAK=1`: the board backs up the FM-1 by itself, job after job; `tools/soak_watch.py` keeps every crash's log and core dump on the Mac. | `f49a08e`, `8414bc7` |

## 1. Ten channels needed, eight available (2026-10-26)

- **Symptom:** the Launchkey enumerated (its name showed) but no MIDI arrived.
- **Cause:** hub (2) + FM-1 on Skiff (EP0 + MIDI 2 + CDC serial 2) + Launchkey (3) = 10.
- **Fix:** the vendored library never claims CDC serial ports (`cb4ee7b`). The screen shows the channel count.

## 2. Allocation inside a critical section (checkpoint 2, 2026-10-27)

- **Symptom:** with packages in the store, the board rebooted about every 3 s.
- **Cause:** the Firmware page's package list was built (`std::string`, `vector::push_back`) inside
  `taskENTER_CRITICAL`. ESP-IDF aborts on allocation with interrupts off.
- **Fix:** build the list first, then copy plain bytes under the lock (`18b5de5`).
- **But the reboots didn't stop.** `231af19`, written after this fix, still describes a build that "reboots every
  ~3 s". There was a second cause.

## 3. Loop task stack overflow (checkpoint 2, 2026-10-27)

- **Found by:** the core dump, the first one decoded in the project.
- **Cause:** `loopTask` at 7,968 of its 8,192 bytes, overflowing inside LVGL's recursive redraw while `loop()` held
  a ~900-byte `UiLoader` on its stack.
- **Fix:** `UiLoader` became static and the loop task got 16 KB (`SET_LOOP_TASK_STACK_SIZE`, `b81773c`).
  Checkpoint 2 then passed on that build.
- **Lesson:** Arduino-ESP32's default 8 KB loop stack is tight once LVGL draws a deep screen. The UI branch was
  warned about the same risk.

## 4. In-place host restarts (checkpoint 3, 2026-10-27 to 10-29)

The architecture said changing mode means restarting the USB host: `usb.end()` then `usb.begin()`, so every device
enumerates again under the new mode's claims. In practice that path failed in several ways:
- an ESP-IDF `hcd_urb_dequeue` assert, twice, with app hub requests in flight;
- the ROM-boot device never seen after the key, though it enumerated the instant the host restarted (`d22c2db`);
- devices that never re-enumerated;
- once, Wi-Fi gone too.

The first job after a fresh boot always worked. So `74033a9` stopped restarting the host in place:
- **A job is its own boot.** The hold records the job in RTC memory (`JobBoot`) and calls `esp_restart()`. The next
  boot starts the USB host straight in Load mode and runs the job, then restarts into Play with the result kept for
  the page.
- **The job is cleared before it runs,** so a crash during it doesn't run it again.

Why `usb.end()/begin()` misbehaves is **open**. One suspect is app hub requests colliding with ESP-IDF's hub driver
on EP0.

## 5. Hub port polls

- **What they were:** the chain screen polled hub port status, one control transfer per port, waiting up to 1 s
  each.
- **Problem:** a timed-out poll abandons its transfer still queued. One `hcd_urb_dequeue` crash came while the loop
  was in `poll_hub_ports()` and the FM-1 was leaving for ROM boot.
- **Fixes:** no polls in Load mode or within 3 s of a host restart (`6b2fbfb`); then none at all in loader builds
  (`3242075`). Branch `hub-poll-timeout` (PR 2) waits 50 ms per port and backs off a silent hub. It waits for
  `loader-session3` to merge.
- **Not the root cause:** crashes continued after the polls were gone (below).

## 6. Hub tracking races

- **What it is:** EspUsbHost's hub tracking opens and closes every unknown device every 500 ms, on the USB client
  task.
- **The race:** in a job boot that runs exactly while the FM-1 leaves USB for ROM boot and the ROM device arrives.
- **Fix:** job boots turn it off (`usb.setHubTrackingEnabled(false)`, `5272934`). Play boots keep it for the chain
  screen.
- **Effect, measured by soak** (counts from the soak logs, see [mistakes](mistakes.md#the-soak-counts)):

  | Build | Finished runs | OK | No ROM device after the key | `hcd_urb_dequeue` crashes |
  |---|---|---|---|---|
  | `a616453`, hub tracking on | 6 | 3 | 3 | 2 |
  | `77ff4ec`, hub tracking off in job boots | 8 | 6 | 2 | 1 |

  Fewer crashes, not none. In both builds a crash sent the board to safe mode, and the soak's next boot retried the
  same run. In the second build, both misses were runs where the FM-1 took the key, left USB and came back in its
  own firmware instead of ROM boot (`fc72748`'s reading). `fc72748` now sends the key again in that case, up to 3
  tries.

## 7. The `hcd_urb_dequeue` assert (open, blocks shipping writes)

- **Where:** `hcd_dwc.c:2661` on the `EspUsbHostClient` task, in `usb_host_client_handle_events` →
  `_handle_pending_ep`.
- **When:** always within about 1 s of the FM-1's disconnect after the ROM-boot key. All three soak crash records'
  breadcrumbs read `usb:disc #2 1209:0001` and `loader:Waiting for ROM boot...`.
- **Never during a write:** every crash came before any erase, so the FM-1 was never at risk from it.
- **Suspects (unconfirmed):**
  - EspUsbHost's disconnect teardown (`handleDeviceGone` → `releaseEndpoints`) while MIDI OUT transfers from
    `midiSend()` are still in flight when the FM-1 leaves after the key.
  - App hub requests colliding with ESP-IDF's hub driver on EP0 (the hub-poll-timeout session's view). Loader
    builds send no hub requests, and the crash still happens.
- **Bar for "fixed":** a soak of at least 20 runs with 0 crashes.

## 8. The FM-1 left, the broker still counted it (checkpoint 1)

- **Symptom:** on one repeat run the FM-1 took the key (its screen went dark) but no usable ROM device appeared
  within 15 s.
- **Suspect:** the broker still counted the FM-1 that had just left ("one FM-1 at a time", or 9 of 8 channels), so
  it refused the ROM device.
- **Workaround:** if the ROM device is there but unclaimed after 3 s, restart the host once and show the broker's
  reason (`536ee16`). Job boots later replaced this: no ROM device 6 s after the key means one board restart and a
  second wait (`74033a9`).
- **Cause:** **open**. The next run passed with no restart needed.

## A harmless failure that looks alarming

Every backup in the soak logs ends with `scsi FC0C addr 00000001 ESP_FAIL 0/16`. That's the reset command: the FM-1
restarts before it answers, so the status phase never arrives. The job doesn't count a reset by its status; it
counts "done" only once the FM-1 is back as a MIDI device (`Step::WaitMidi` in `installer.cpp`). In the soak logs
`1209:0001 Felucca` reconnects about 0.8 s after the reset. Checkpoint 1 confirmed that `0xFC0C` with argument 1
restarts the FM-1 into its firmware.
