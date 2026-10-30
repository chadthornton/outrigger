# USB MIDI host on the ESP32-S3

[Index](index.md) · next: [What broke in the USB host](usb-host-failures.md)

The ESP32-S3 can be a USB host, but only a small one: full speed, 8 host channels, and an ESP-IDF host stack that
Arduino reaches through a library. Most of what this project learned about USB comes down to one budget (8
channels) and one rule (decide every claim before the library makes it).

## The rig

| Part | What it is | Finding |
|---|---|---|
| Host | Waveshare ESP32-S3-Touch-LCD-1.69 | Its one USB-C port is wired to the S3's native USB, so it can host. |
| Hub | Genesys Logic `05e3:0610`, USB 2.0, powered | Its PD pass-through powers the Waveshare up the upstream cable, so the whole rig runs from one supply. |
| Synth | M-VAVE FM-1 (on Skiff firmware) | On a hub USB-C port. |
| Controller | Novation Launchkey Mini MK4 37 (`1235:0142`) | On a hub USB-C port. |

Source: README, "Hardware findings (2026-10-26)". Milestones 1–3 (FM-1 MIDI on screen, both devices together,
Launchkey plays the FM-1 with system messages blocked) all landed on 2026-10-26.

**The hub's USB-A ports don't work.** The hub is internally two cascaded 4-port hub chips. On the Waveshare only
the outer chip comes up; the inner chip's port shows connected but is never enabled. The USB-C ports are on the
outer chip and work; the USB-A ports are on the inner one. Why the inner chip never enables is **open**.
EspUsbHost's own README says internally cascaded hubs "are not fully validated".

## The library: a vendored, patched EspUsbHost

`lib/EspUsbHost` is a copy of [EspUsbHost](https://github.com/tanakamasayuki/EspUsbHost) 2.10.1 (MIT, by
tanakamasayuki). It was picked because one Arduino library covers USB MIDI, hubs, mass storage and audio on the S3.
The copy is vendored rather than installed because it carries patches. Each is marked `usb-midi-host patch` in the
source:

| Patch | Why | Commit |
|---|---|---|
| `ESP_USB_HOST_MAX_SERIAL_PORTS` overridable; the Makefile sets it to 0 | Skiff exposes a CDC serial port. Claiming it cost 2 channels, so hub + FM-1 + Launchkey needed 10 of 8 and the Launchkey enumerated with no MIDI. | `cb4ee7b` |
| `setClaimPlanner()` and `setClaimFilter()` | Unpatched, the library claims the first MIDI, audio and mass-storage interface of every device, with no hook. The planner sees each device's raw configuration descriptor before any claim; the filter can veto each claim. | `c5e8672` |
| `mscRawCommand()` | One raw SCSI command to a mass-storage device, for the JieLi ROM bootloader. It sends no GET_MAX_LUN, inquiry, capacity, REQUEST SENSE or Bulk-Only reset of its own, always reads the status (CSW), and on failure only clears halted pipes. | `cdba52f` |
| `MscRawResult` | Why the last raw command failed (stage, transfer status, CSW status, residue, bytes moved), for the diagnostics log. Records allocation failure as `-3`. | `2d5ae67`, `18ad445` |

Two upstream features also matter: `setHubTrackingEnabled(false)` (see [failures](usb-host-failures.md#6-hub-tracking-races))
and the controller FIFO split in `EspUsbHostConfig::fifo` (below).

**Library facts that shaped the code:**
- **Every callback runs on the library's USB client task.** Mass-storage calls are refused from that task, so the
  loader runs on its own task. LVGL may only run in `loop()`.
- **"Connected" comes before "claimed".** EspUsbHost reports a device as connected before its interfaces are
  claimed. The first ROM-boot attempt sent its query too early and failed ("couldn't send to the FM-1"). The loader
  now waits until the FM-1's MIDI is ready and the broker says the Loader owns it (`e848423`).
- **Iso packets with errors are skipped silently.** A recorder has to count frames against elapsed time to find
  dropouts (`docs/audio-recording-spike.md`).

## The 8-channel budget

The S3's USB controller has 8 host channels (`soc/usb_dwc_cfg.h`; EspUsbHost's header gives 8 for the S2, the S3 and
the P4's full-speed port, and 16 for the P4's high-speed port). Each open pipe holds one.

**How to count** (`docs/architecture.md`, "Channel cost"): EP0 for every device, hubs included, plus one per
non-control endpoint of each claimed interface.

| Device | Interfaces claimed | Channels |
|---|---|---|
| Hub | hub (interrupt IN) | 2 |
| Launchkey | audio control, first MIDI interface (bulk IN + OUT) | 3 |
| FM-1, playing | audio control, MIDI | 3 |
| FM-1, recording | audio control, audio streaming (iso IN) | 2 |
| FM-1 in ROM boot | mass storage (bulk IN + OUT) | 3 |
| Skiff's CDC serial pair, HID, second MIDI interfaces | never | 0 |

Hub + FM-1 + Launchkey is exactly 8. A third device doesn't fit.

**What over budget looks like:** the device enumerates (its name appears) but delivers no MIDI. Nothing in the
library says why. The screen's top line now shows the count as `N/8`, and a refused device gets a two-line reason.

**The audio interface and the FIFO.** EspUsbHost also claims a device's audio streaming interface. On `master` that
claim fails silently: the Arduino core's prebuilt FIFO split caps IN endpoints below the FM-1's 196-byte iso
packets. The audio spike fixed it with `fifo = {120, 48, 32}` (200 lines in all; a 224-line split stopped the host
from starting). That fix would push the playing rig to 9 of 8, so it stays on `spike/audio` until the broker leaves
the FM-1's audio unclaimed in Play (`docs/audio-recording-spike.md`).

**Open:** whether a device whose interfaces are all refused still holds its EP0 channel. The broker assumes it
does.

## The resource broker

Several apps want the same 8 channels and the same FM-1: the Router (MIDI), the Loader (SysEx key, then ROM boot)
and the Recorder (audio). `lib/platform/src/broker.h` decides, per interface, whether the host claims it and which
app may use it. It is plain C++ with 22 Mac tests (`tests/test_broker.cpp`).

1. **`attach(device)`** plans a whole device once it has enumerated (its EP0 already costs a channel): which
   interfaces, for which app, at what cost. If the device ends up doing nothing, it also writes the two screen
   lines that explain why.
2. **`should_claim(address, interface)`** answers the patched library's claim filter.
3. **`set_mode(mode)`** forgets every device. A claim can't be taken back without re-enumerating.
4. **`owner(part)`** says which app may act on the FM-1's MIDI, audio or ROM-boot face.
5. **`wanted_mode()`** asks for Load mode when an FM-1 already in ROM boot appears (the loader's resume entry).

**Rules worth copying:**
- **Hold the FM-1's share.** While no FM-1 is attached, other devices may not claim into the channels an FM-1 would
  need, but only while that share can still fit. Once a third device has enumerated its EP0 is spent anyway, so
  refusing it gains nothing.
- **Explain only devices that do nothing.** Each reason is two lines of at most 33 characters, and a test checks
  that every refusal × kind × mode combination fits. Example: `No USB channel for MPK` / `8 of 8 in use: unplug a device`.
- **Load mode is structural.** In Load mode the controllers' MIDI interfaces aren't even claimed, so nothing can be
  routed while the loader runs.

The first review of the broker found that a hub's interrupt pipe must always be counted, because ESP-IDF's hub
driver opens it whatever the claim filter says (`86da5ee`).

## Class compliance and routing

The FM-1 and the Launchkey are both class-compliant USB MIDI devices; nothing device-specific is needed to play.
The Router (`lib/midi_core`, `route_to_target()`) forwards only channel voice messages (`0x80`–`0xEF`) from the
controller's cable 0, and drops every system message, SysEx included. So no controller can deliver an install or
ROM-boot command to the FM-1. Tests in `tests/test_midi_core.cpp` cover this.

One lesson about guessing: `status.cpp` used to fall back to "It may not be class-compliant" for a device whose
MIDI didn't open. It showed for a Launchkey whose MIDI simply didn't reopen after many host restarts. The fallback
no longer guesses (`605c522`).

## Wi-Fi beside the USB host

The web-UI spike measured Launchkey → FM-1 forwarding with Wi-Fi off, idle, and hammered by a web page: the
worst case grew from 0.41 ms to 0.71 ms, and all 2,097 notes arrived (`docs/web-ui-spike.md` on branch
`spike/web-ui`). Wi-Fi can stay on. Two radio lessons from the same spike:
- **One radio:** joining a network moves the setup hotspot's channel and drops the phone, so the page replies
  first and joins second.
- **mDNS on macOS** asks for an IPv6 address first. Without link-local IPv6, `.local` took 5 s from `curl`.

The web diagnostics that later landed (`2d5ae67`) run `httpd` on core 0 at priority 3, below the USB host's 5, and
never touch EspUsbHost: the loop publishes copies under a mutex, and each task logs to its own lock-free ring.

## If you're building one

- **Count channels before buying devices.** Two MIDI devices and a hub is the S3's limit.
- **Turn off what you don't use.** CDC serial ports and audio interfaces cost channels you don't see.
- **Use a powered hub.** Hub + Waveshare + FM-1 + controller from one brick worked here.
- **Test the hub's ports one by one.** On this hub only half of them worked.
- **Give the library a claim filter** and decide claims from the configuration descriptor, not from VID:PID guesses.
