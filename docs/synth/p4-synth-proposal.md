# Outrigger Synth on the ESP32-P4 3.5": proposal, no pin work (2026-11-01)

> **Status, 2026-11-05: shelved after step 1.** See [SHELVED.md](SHELVED.md) to resume.
>
> The probe session ("Outrigger Synth: P4 USB probe", results in [p4-usb-probe.md](p4-usb-probe.md), sketch on
> branch `claude/synth-p4-probe-05de29`) ran Chad's spare powered hub on the OTG port in forced-full-speed mode.
>
> **Step 0, the USB probe:**
>
> | Check | Result |
> |---|---|
> | Hub | Genesys `05e3:0610` enumerated at full speed |
> | Controller | A Launchkey MK3 37 (`1235:0135`) behind it logged notes |
> | Audio output | An **iFi USB Audio SE** (`2fc6:f082`, UAC2) offered startable 44.1–96 kHz stereo output |
> | Channels | 11 of 16 used |
> | Replugs | 10 software replugs, all identical |
>
> **Step 1, the tone soak:** a 440 Hz tone sounded clean by ear (Chad) and ran 600 s with **0 underruns**, then
> 13.5 h unattended, still 0, with every timing-feedback report from the DAC accepted. Still open: notes played
> *during* the tone.
>
> **What changed from this proposal:**
> - **No C-Media adapter needed.** The iFi works once EspUsbHost's FIFO preset for larger isochronous OUT packets is
>   set: its packets are 776 bytes, and the default is 512.
> - **Hub first, then POWER.** Plugging the hub in switches the board off, and pressing POWER brings it back.
> - **Replugs:** a few real cable pulls would still back up the software replugs.
>
> **Next:** step 2, Felucca rendering into the DAC from the Launchkey, with render times measured.

**Proposal:** run the synth on the **ESP32-P4-WIFI6-Touch-LCD-3.5** with everything on the powered hub you already
use:
- the Launchkey for keys and knobs;
- a **~$10 C-Media USB sound adapter** for headphones or line out.

It all goes into the board's one USB OTG port. No soldering, no header wires. If the USB sound adapter fails, the
board's built-in speaker is the fallback.

**Two things to prove before building, both cheap:**
1. **Does a powered hub work on this port?** EspUsbHost has an experimental full-speed mode for exactly this. It's
   verified on the bare port but not yet through a hub. This is a 1–2 session probe, and it's the same test Board
   support already planned for this board.
2. **Does a C-Media adapter play audio from EspUsbHost?** Its USB-audio output is experimental: it passes tests
   against other ESP32 boards acting as audio devices, but nobody has validated it on a real sound card.

**Size if both pass:** about 3–4k lines, roughly 3–5 weeks of sessions. That's the P4 port of Outrigger's core, a
USB audio output, Felucca and a synth screen.

---

## What the board really has (verified this time)

My earlier §7 note guessed at the USB wiring. These facts come from Waveshare's repo
(`waveshareteam/ESP32-P4-WIFI6-Touch-LCD-3.5`), its README and its schematic.

- **Two USB-C ports, but only one can host.**
  - **"USB OTG" port:** wired to the P4's high-speed USB pins (`USBD_P/N`). This is the one for the hub.
  - **"UART" port:** a CH343P USB-to-serial chip, for flashing and logs. It can't host devices or carry audio.
  - **DP/DM on the header:** the P4's second USB controller (GPIO24/25, the built-in serial/JTAG by default). Using
    it means wiring, so it's out.
- **So "one USB out for the DAC" isn't available.** The DAC has to share the hub with the Launchkey.
- **The board doesn't power USB devices.** The OTG port has device-style 5.1 kΩ pull-downs on CC, and its 5 V comes
  *in* through a MOSFET to power the board. A dongle plugged straight into the board gets no power. A **powered hub**
  is required, the same as Outrigger's setup today.
- **Also on the board** (product page and silkscreen):
  - 768 KB internal RAM and 32 MB PSRAM in the chip package;
  - the same ST7796 screen and FT6336 touch as the S3 3.5";
  - an ES8311 codec with a speaker connector and a mic;
  - an AXP2101 power chip;
  - an ESP32-C6 that provides Wi-Fi.

## The hub problem, and the workaround

**The problem** (primary sources):
- ESP-IDF's USB host guide says its external-hub driver has no "transaction translator". So with a **high-speed hub
  on a high-speed port, full-speed devices behind the hub aren't supported**.
- Espressif's controller notes add that the P4's high-speed controller has no hardware support for those split
  transactions either.
- The Launchkey, the FM-1 and C-Media dongles are all full-speed devices.
- So a normal hub on this port fails. That's why EspUsbHost calls hubs "effectively not usable" there.

**The workaround:** EspUsbHost 2.10.1, the version Outrigger already vendors, has `experimentalForceFullSpeed`.
- It makes the high-speed port run the whole bus at **full speed** (12 Mbit/s). The hub then shows up as a
  full-speed hub, and full-speed devices behind it talk normally, with no translator needed.
- That's the speed Outrigger's S3 boards run at today, so nothing is lost.
- The library's notes (`docs/p4-hs-port-fs-only-hub.ja.md`) record:
  - **Verified on hardware, 2026-09-13:** the bare port came up at full speed (1.196 MB/s, 98% of the full-speed
    limit).
  - **Not verified:** a hub on the port. Their test plan's first three items are exactly our case: hub alone, hub +
    keyboard, hub + another device.
  - **Known gap:** if the USB controller hits an internal error and resets, ESP-IDF may drop the full-speed setting.
    Likely symptom: the hub disappears until a reboot. Fixing it needs an ESP-IDF change.
- **Channels:** the P4's high-speed controller has **16** host channels, against the S3's 8 (from Board support's
  notes; confirm in the probe). Forcing full speed keeps the same controller. Hub + Launchkey + FM-1 + DAC fits with
  room left over.

## The USB sound adapter

- **It must be a full-speed USB Audio Class 1 device.** Look for a **C-Media CM108 / CM119**-based "USB sound card"
  (USB-A plug, about $8–15). Both chips' datasheets state "USB 2.0 Full Speed, USB Audio Device Class 1.0". On a
  computer it shows up with vendor ID `0d8c`.
- **Avoid:**
  - "hi-res" or 24/192 dongles, which are often high-speed-only and won't appear at all on a full-speed bus;
  - Apple's USB-C adapter: its bus speed is unverified, so don't count on it.
- **Quality:** CM108-class sound is like a decent laptop headphone jack. That's fine for a synth.
- **Software:** EspUsbHost supports UAC1 and UAC2 output.
  - It finds the stream, sets the rate and sends packets, and it reads the clock feedback from devices that send it.
  - It defaults to 48 kHz. We'd ask for **44.1 kHz**, since Felucca's tables are built for that rate.
  - It's peer-tested only, so the probe is the real test.
- **Latency:** USB audio adds a small buffer on top of the synth's own. Aim for under 10 ms from key to sound, and
  measure it. The FM-1 cartridge project modeled about 25 ms for its longer path, as an upper reference.
- **Fallback with no extra hardware:** the onboard ES8311 speaker. It's mono and small, but needs no dongle.

## Parts list

| Part | Status |
|---|---|
| ESP32-P4-WIFI6-Touch-LCD-3.5 | have |
| Powered USB hub, and the cable from the board's **OTG** port to it | have |
| Launchkey | have |
| C-Media CM108/CM119 USB sound adapter | ~$10 |
| Headphones or powered speakers | — |

Flashing uses the board's **UART** USB-C port from the Mac, a second cable during setup only.

## How it uses Outrigger

**Kept as-is or nearly:**
- **EspUsbHost.** The same vendored library already supports P4 port choice, forced full speed and USB audio output.
- **Router** (`lib/midi_core`). The Launchkey → target routing stays. The new piece is an **internal synth target**
  next to the FM-1, so the same chain screen shows "Launchkey → Synth".
- **Resource broker** (`lib/platform`). It already decides which device gets which USB interface. Add the DAC's
  audio output as a claim, raise the budget to 16 channels for the P4, and give the synth the flash-write lease that
  pauses audio during saves.
- **Screen shell** (`lib/host_ui`, LVGL) and the board-profile system (`firmware/boards.json`).
  - The display and touch chips match the S3 3.5", so the drivers carry over.
  - Only the pins and the chip family in the profile change.
- **The audio rule from the recorder spike** ("ask for 44.1 kHz explicitly"). It applies to the DAC the same way.
- **Keep, but leave out of version 1:** the FM-1 Loader and the web diagnostics.

**New:**
- **A P4 build target:** arduino-esp32 supports the P4, with its own board setting, partition table and RISC-V
  toolchain. The installer site would need a P4 entry later.
- **USB setup:** the high-speed port plus `experimentalForceFullSpeed`, one config block.
- **Audio engine task:** Felucca's host layer, as in §6 of synth-engine-scope.md.
- **One "audio out" interface with two backends:**
  - the **USB DAC** (EspUsbHost `audioOutputStart` / `audioSend`);
  - the **ES8311 speaker** (I2S).

  It picks the DAC when one is plugged in, otherwise the speaker.
- **Synth screen:** Felucca's 240×240 display shown beside an on-screen panel on the 480×320 landscape screen, with
  Launchkey knobs and pads mapped to the FM-1's knobs and buttons.
- **Storage:** Felucca's 1 MiB "flash" goes in a partition, saved only while stopped.

**Different, deferred:**
- **Wi-Fi** comes through the ESP32-C6 co-processor ("esp-hosted"), a different stack from the S3's. Leave it out of
  version 1.

## Steps

| # | Step | Pass when | Sessions |
|---|---|---|---|
| 0 | **USB probe.** A minimal P4 build: OTG port, forced full speed, powered hub with the Launchkey and the C-Media adapter. | The hub reports full speed; Launchkey notes arrive; the adapter appears with a 44.1 kHz stereo output; channel count logged. Replug 10 times with the same result. | 1–2 |
| 1 | **Tone to the adapter.** 440 Hz through `audioOutputStart(2, 16, 44100)` while the Launchkey plays. Also the same tone on the speaker. | Clean tone in headphones; 10-minute soak with no dropouts; notes still logged. | 1–2 |
| 2 | **Felucca from the Launchkey,** rendered into the adapter. | ANALOG chords play; render time per block logged for 1, 4 and 8 notes, plus FM6 and GRAIN; no dropouts. | 2–3 |
| 3 | **Synth app:** Felucca's screen, knobs and buttons mapped, saves while stopped. | Plays and edits as an FM-1 would, with no FM-1. | 4–6 |
| 4 | Optional: the FM-1 on the same hub as a second sound source. | — | later |

**Stop points:**
- **Step 0 fails:** the P4 can't host our hub today. Use the S3 3.5" for the synth, through its speaker or an S3 DAC
  path, and watch EspUsbHost and ESP-IDF releases.
- **Step 1 fails but step 0 passes:** use the onboard speaker, and report the adapter to EspUsbHost with its VID:PID.

## Risks

1. **The hub in forced full-speed mode is unverified.** It's the gate, and step 0 tests it directly.
2. **USB audio output from EspUsbHost on a real adapter is unverified,** including isochronous audio through the hub.
3. **Recovery may fall back to high speed** after a controller error. Likely symptom: the hub drops until a reboot.
   It needs an ESP-IDF change for a permanent fix.
4. **New platform work:** P4 support in Outrigger's build, installer and board profiles. EspUsbHost's own P4
   validation is still in progress.
5. **CPU and memory are much better than on the S3,** but still estimates until step 2 measures them.

## Correction to synth-engine-scope.md §7

§7 suggested this board might act as a USB audio device to a computer through its second USB controller. **That
needs wiring on this board:** the second controller's pins reach only the header, not a USB-C port. Treat it as not
available without pin work.

## Sources

- Board:
  - Waveshare repo: https://github.com/waveshareteam/ESP32-P4-WIFI6-Touch-LCD-3.5. `README.md` says "USB-UART
    Type-C and USB 2.0 High-Speed OTG Type-C".
  - Schematic: `schematic/ESP32-P4-WIFI6-Touch-LCD-3.5-schematic.pdf`. The USB OTG block (H1, CC 5.1 kΩ to GND,
    USB0_5V → Q2 AO3401 → VCC_5V), the CH343P, and USB1P1_P/N on GPIO25/24.
  - Product overview pasted by Chad.
- EspUsbHost 2.10.1:
  - `lib/EspUsbHost/README.md`: ESP32-P4 notes, Audio scope.
  - `src/EspUsbHost.h:338-343`, `src/EspUsbHost.cpp:1740, 8692-8735`.
  - Upstream `docs/p4-hs-port-fs-only-hub.ja.md` (https://github.com/tanakamasayuki/EspUsbHost).
- ESP-IDF:
  - USB Host Programming Guide, ESP32-P4: https://docs.espressif.com/projects/esp-idf/en/latest/esp32p4/api-reference/peripherals/usb_host.html
  - DWC_OTG maintainers' notes: https://docs.espressif.com/projects/esp-idf/en/latest/esp32p4/api-reference/peripherals/usb_host/usb_host_notes_dwc_otg.html
  - Both cited through the EspUsbHost document.
- C-Media datasheets:
  - CM108B: https://wmsc.lcsc.com/wmsc/upload/file/pdf/v2/lcsc/1902141615_Cmedia-CM108B_C371347.pdf
  - CM119: https://www.repeater-builder.com/voip/pdf/cm119-datasheet.pdf
  - Adafruit notes on CM108 dongles: https://learn.adafruit.com/usb-audio-cards-with-a-raspberry-pi/cm108-type
- Community UAC2 host driver (S3, miniDSP-tested): https://components.espressif.com/components/averyy/usb_host_uac2/versions/0.2.0/readme
- Outrigger: `docs/architecture.md`, Board support's `docs/boards.md` (ESP32-P4 section).
