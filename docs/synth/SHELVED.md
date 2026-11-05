# Outrigger Synth: shelved (2026-11-05)

Chad shelved the synth track after step 1: "excited about it, but shelve it". This page says what was proven,
where everything is, and how to pick it up again.

## What was proven

**On Big-S3** (Waveshare ESP32-S3 3.5", MAC `44:bd:8d:61:2b:28`, 2026-11-03), from PR #54 and its bench. The
full table is in [s3-first-sound.md](s3-first-sound.md).
- **Felucca fits on one S3 core.** All 13 engines' first presets with 1, 4 and 8 notes held had 0 underruns and 0
  blocks over the 1,451 µs budget (64 frames at 44.1 kHz). The worst case was PHYS (BELL TREE) at 1,028 µs, 71% of
  the budget. Most engines were at 14–54%.
- **I2S tone, board side only:** 647 s at 48 kHz with 0 underruns. It was never heard: Big-S3 has a speaker
  connector but no speaker.

**On Big-P4** (Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5, rev v3.2, MAC `e8:f6:0a:e8:a5:55`, 2026-11-01 to 11-03).
The details are in [p4-usb-probe.md](p4-usb-probe.md).
- **Step 0, the USB probe, passes.**
  - **Hub:** with EspUsbHost's `experimentalForceFullSpeed`, a Genesys `05e3:0610` hub on the OTG port enumerates
    at full speed. This is the first hardware confirmation of the hub path.
  - **Devices behind it:** a Launchkey MK3 37 logs notes, and an iFi USB Audio SE (`2fc6:f082`, UAC2) offers a
    startable 44.1 kHz, 16-bit stereo output.
  - **Channels:** 11 of 16 host channels are in use.
- **Step 1, tone into the USB DAC, passes on audio.**
  - **The tone:** 440 Hz at 44.1 kHz through `audioOutputStart(2, 16, 44100)` and `onAudioOutputRequest`.
  - **By ear:** clean (Chad).
  - **Soak:** 0 underruns over 13.5 h, and the host paced to the DAC's clock feedback with 0 rejects.
- **The large periodic-OUT FIFO is required.**
  - **Why:** the iFi's isochronous OUT endpoint is 776 bytes, and the driver's default FIFO split caps periodic OUT
    at 512. So it failed with `EP MPS (776) exceeds supported limit (512)`.
  - **The fix:** `config.fifo = ESP_USB_HOST_FIFO_LARGE_PERIODIC_OUT`, which is `{260, 128, 280}` lines and valid
    only on the P4 high-speed port.
- **A software replug works through `usb.end()` then `usb.begin()`.**
  - **Result:** 10 of 10 cycles came back identical.
  - **Full speed holds:** `FSLSSUPP=1` is re-applied on every `begin()`.
- **Toggling root-port power does not work as a replug.**
  - **What happens:** `usb_host_lib_set_root_port_power(false)` drops the devices behind the hub. But EspUsbHost
    still holds the hub's handle, so the hub never disconnects.
  - **Result:** `set_root_port_power(true)` then returns `ESP_ERR_INVALID_STATE` every time.

## Where everything is

**On master:**
- `lib/synth/` (PR #54 "first sound"):
  - `felucca_unit.c`: Felucca 1.7.0 through the fm1-cartridge port, GPL-3.0, vendored under `third_party/`.
  - `synth_engine`: the audio task.
  - `audio_out`: the S3's ES8311 and I2S output.
  - `es8311/`: the ES8311 codec driver.
- `firmware/synth_bench/`: the S3 bench (`make synth-bench`).
- The dev-only `synth` app set: `make build BOARD=waveshare_35 APPS=synth`, built by CI and never by `make site`.
- `Router::to_synth`.
- `docs/synth/`: this folder.

**Only on a branch:**
- `claude/synth-p4-probe-05de29` at `c10f29a` holds `prototypes/p4_usb_probe/`: the P4 probe sketch (`probe.cpp`)
  and `serial_log.py`, an unattended console logger that takes commands from a file.
- PR #49's docs (README, proposal, engine scope) moved into this folder; #49 is closed as superseded.
- The S3 bench's raw `soak.log` and `sweep.log` stay in the orchestrator's side-session notes folder. Its write-up
  is [s3-first-sound.md](s3-first-sound.md).

## Open questions

1. **Notes during audio.** No one played the Launchkey while the tone ran, so MIDI and audio under load together
   is untested. Step 2 covers it.
2. **The board dropping off at 05:55.** After 13.5 h of tone, Big-P4 vanished from the Mac with nothing in the log.
   The cause could be power, the Mac sleeping its ports, or a crash. It's unknown.
3. **The hub switching the board off.** Plugging the powered hub into the OTG port turns Big-P4 off, and pressing
   POWER brings it back. The likely cause is the USB-C hub sending 5 V back into the board's OTG power input, which
   isn't confirmed against the schematic.
   - **Workaround for now:** hub first, then POWER.
   - **Untested alternative:** power the board from the hub alone.
4. **Physical replugs.** Only software replugs were run. Because of question 3, every cable pull also needs a POWER
   press.
5. **Which shell for the synth app (step 3).**
   - **Felucca's own host** (`platforms/host/felucca_host.c`): an FM-1 without an FM-1.
   - **fm1-base's `plat.h`:** a platform for FM-1 apps.

   The first-sound notes lean towards `plat.h`. Decide after step 2's render numbers on the P4.
6. **The ES8311 speaker on the P4,** for when no DAC is attached. Not tried.

## How to resume

1. **Read [p4-synth-proposal.md](p4-synth-proposal.md) next.** It has the plan, the board's USB facts and steps 2
   to 4. Then read [p4-usb-probe.md](p4-usb-probe.md)'s "For step 2" section.
2. **Board: Big-P4** (`e8:f6:0a:e8:a5:55`). Book it through the orchestrator, because it's back in the free pool.
   Check the MAC with `esptool --port <port> chip-id` before writing to it.
3. **Wiring:**
   - The Mac goes to the board's **UART** USB-C: the CH343P, which shows up as `/dev/cu.usbmodem…`.
   - The powered hub goes to the **USB OTG** USB-C, then press **POWER**.
   - On the hub: the Launchkey and the iFi.
4. **Build:**
   - FQBN: `esp32:esp32:esp32p4:DebugLevel=info,PSRAM=enabled,ChipVariant=postv3`, with arduino-esp32 3.3.12 and
     `--build-property tools.ctags.path=/opt/homebrew/bin`.
   - Rev v3.2 silicon needs `postv3`.
   - Never use `DebugLevel=verbose`: it crashes the console while USB host runs.
5. **USB config:**
   - `port = ESP_USB_HOST_PORT_HIGH_SPEED`;
   - `experimentalForceFullSpeed = true`;
   - `fifo = ESP_USB_HOST_FIFO_LARGE_PERIODIC_OUT`.
6. **To rerun the probe:** `git checkout c10f29a -- prototypes/p4_usb_probe`. Its serial commands are `t` (tone),
   `s` (stop), `r` (report) and `c` (10 software replugs).
7. **Step 2:**
   - Felucca from the Launchkey into the iFi, reusing `lib/synth`'s `felucca_unit.c` and `synth_engine`.
   - Swap the I2S output step for a ring buffer, which `onAudioOutputRequest` drains at about 44 frames per 1 ms
     USB frame.
   - Measure render µs per block on the P4.
