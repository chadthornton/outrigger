# P4 USB probe: hub, controller and USB DAC on the ESP32-P4 3.5" (2026-11-01)

Steps 0 and 1 of the Outrigger Synth P4 proposal. **Step 0 passes; step 1 passes on audio, with one gap: no notes were played during the soak.** The P4's
high-speed port, forced to full speed, runs a high-speed hub with a MIDI controller and a USB DAC behind it, both
claimed at once, and a 440 Hz tone plays cleanly at 44.1 kHz.

Sketch: `prototypes/p4_usb_probe/` on branch `claude/synth-p4-probe-05de29` (`probe.cpp`; `serial_log.py` logs the
console unattended). It isn't on master; [SHELVED.md](SHELVED.md) says how to get it back.

## Setup

| Part | Detail |
|---|---|
| Board | Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5, ESP32-P4 **rev v3.2**, MAC `e8:f6:0a:e8:a5:55` |
| Core / library | arduino-esp32 3.3.12, vendored EspUsbHost 2.10.1 |
| Build | `esp32:esp32:esp32p4:DebugLevel=info,PSRAM=enabled,ChipVariant=postv3` (rev 3.x needs `postv3`) |
| USB config | `port = ESP_USB_HOST_PORT_HIGH_SPEED`, `experimentalForceFullSpeed = true`, `fifo = ESP_USB_HOST_FIFO_LARGE_PERIODIC_OUT` |
| Hub | Genesys Logic "USB2.1 Hub" **`05e3:0610`**, powered, USB-C upstream to the board's **USB OTG** port |
| Controller | Novation Launchkey MK3 37 **`1235:0135`** |
| DAC | iFi USB Audio SE **`2fc6:f082`** (UAC2, maker string "SXW"). Not the C-Media CM108/119 the proposal named; Chad had this one |
| Logs | Board's **UART** USB-C (CH343P) to the Mac, 115200 |

## Results

| Item | Result | Evidence |
|---|---|---|
| Hub enumerates at full speed | **PASS** | `speed=full`, `protocol=0x00` (a full-speed hub), `FSLSSUPP=1` |
| Controller's notes logged | **PASS** | 94 note on/off events through the hub (channel 3) |
| Adapter has a startable 44.1 kHz stereo output | **PASS** (needed the large periodic-OUT FIFO) | UAC2 alt 1: 2 ch, 16 bit, 44.1/48/88.2/96 kHz, `startable=1` |
| Channel use logged against 16 | **PASS** | 11 of 16: hub 2, Launchkey 6, iFi 3 |
| 10 replugs, same result | **PASS (software)** | 10/10 `usb.end()`/`usb.begin()` cycles identical; see the caveat below |
| Step 1: 440 Hz tone, clean in headphones | **PASS** | Chad: "I hear the tone. No clicks or anything else." |
| Step 1: 10 minutes, no dropouts | **PASS** | 0 underruns at 600 s; the tone then ran on unattended for **48,560 s (13.5 h)**, still 0 underruns and 0 feedback rejects |
| Step 1: notes logged while the tone plays | **NOT COVERED** | No notes arrived during the soak; notes were only logged before the tone started (16:15) |

### Log excerpts

Host start, forced full speed:

```
[EspUsbHost] Experimental P4 HS-port full-speed-only mode: HCFG=0x00000204 FSLSSUPP=1
HOST_READY port=hs bus_mode=full_speed_only
```

Settled topology:

```
DEVICE addr=1 parent=0 port=0x01 speed=full hub=1 class=0x09 protocol=0x00 vid:pid=05e3:0610 usb=2.10 product="USB2.1 Hub" maker="GenesysLogic"
CHANNELS addr=1 ep0=1 endpoints=0 hub_endpoints=1 total=2
DEVICE addr=2 parent=1 port=0x12 speed=full hub=0 class=0xef protocol=0x01 vid:pid=2fc6:f082 usb=2.00 product="iFi USB Audio SE" maker="SXW"
CHANNELS addr=2 ep0=1 endpoints=2 hub_endpoints=0 total=3
AUDIO_STREAM addr=2 if=1 alt=1 ep=0x01 dir=out uac=2 ch=2 bits=16 mps=776 startable=1 rates=44100,48000,88200,96000
AUDIO_STREAM addr=2 if=1 alt=2 ep=0x01 dir=out uac=2 ch=2 bits=24 mps=776 startable=0 rates=44100,48000,88200,96000
DEVICE addr=3 parent=1 port=0x13 speed=full hub=0 class=0x00 protocol=0x00 vid:pid=1235:0135 usb=2.10 product="Launchkey MK3 37" maker="Focusrite - Novation"
CHANNELS addr=3 ep0=1 endpoints=5 hub_endpoints=0 total=6
CHANNELS used=11 of=16
PROBE hub_full_speed=1 hub_high_speed=0 downstream_fs=1 midi_ready=1 audio_out_44k1_stereo=1
```

Notes:

```
NOTE addr=2 ch=3 on note=54 vel=42
NOTE addr=2 ch=3 on note=51 vel=22
NOTE addr=2 ch=3 off note=51 vel=0
```

Replugs:

```
CYCLE baseline devices=3 channels=11 hub_fs=1 down=1 midi=1 out44k=1
CYCLE 1/10 end
CYCLE 1/10 begin=ok HCFG=0x00000204 FSLSSUPP=1
CYCLE 1/10 result=same devices=3 channels=11 hub_fs=1 down=1 midi=1 out44k=1
...
CYCLE done same=10 of=10
```

Tone (`audioOutputStart(2, 16, 44100)`, filled through `onAudioOutputRequest`):

```
[EspUsbHost] USB Audio feedback polling started: ep=0x81 size=4 nominal=44100
TONE start=ok hz=440 rate=44100 feedback=1
TONE t=60s running=1 frames=2646840 underruns=0 (+0) pace_rate=44100 fb_updates=7501 fb_rejects=0
TONE t=600s running=1 frames=26460826 underruns=0 (+0) pace_rate=44100 fb_updates=75001 fb_rejects=0
...
TONE t=48560s running=1 frames=2141494895 underruns=0 (+0) pace_rate=44100 fb_updates=6070001 fb_rejects=0
LOGGER lost /dev/cu.usbmodem5CF70432541        (05:55:03 next morning; board off the Mac, cause not seen)
```

## Findings

1. **The DAC needs the large periodic-OUT FIFO.** The iFi's isochronous OUT endpoint is 776 bytes. The driver's
   default FIFO split caps periodic OUT at 512, so every audio alternate failed to claim:
   `HCD DWC: EP MPS (776) exceeds supported limit (512)`. `config.fifo = ESP_USB_HOST_FIFO_LARGE_PERIODIC_OUT`
   (`{260, 128, 280}` lines, valid only on the P4 HS port) fixes it. A CM108-class adapter (48 kHz/16-bit
   stereo, about 200-byte packets) would fit the default, but the synth app should set this anyway.
2. **Use `onAudioOutputRequest`, not `audioSend`.** The task named `audioSend`. The README calls that
   low-level and recommends the callback for continuous playback, and the callback also gets the device's clock
   feedback. The iFi is asynchronous with a feedback endpoint; the host paces to it with zero rejects.
3. **Only the first audio alternate is claimed.** The 24- and 32-bit formats show `startable=0`. That's fine
   for the synth's 16-bit output.
4. **Plugging the hub in switches the board off.** It happened twice: the green power LED goes out and the
   CH343 disappears from the Mac. Pressing POWER (AXP2101) brings it back with the hub attached, and it stays
   on. The likely cause is the powered USB-C hub passing 5 V back up its upstream port, into the board's
   OTG 5 V input, while the Mac powers the UART port. Not confirmed against the schematic. For the synth:
   connect the hub, then power on, or power the board from the hub alone (still to test).
5. **Hub port 4 has a device that can't enumerate at full speed**, with nothing plugged in:
   `ENUM: [1:4] CHECK_SHORT_DEV_DESC FAILED`. Probably something built into the hub. Harmless.
6. **Root-port power toggling is not a usable replug.** `usb_host_lib_set_root_port_power(false)` drops the
   downstream devices, but the hub stays tracked and `set_root_port_power(true)` returns
   `ESP_ERR_INVALID_STATE` every time. `usb.end()`/`usb.begin()` works and re-applies `FSLSSUPP` each time
   (upstream test matrix item 7).
7. **Board revision v3.2** needs `ChipVariant=postv3`; the default build targets pre-v3 silicon.
8. **The iFi's HID interface (its buttons) fails to claim** (`HCD DWC: bInterval value (0) invalid for pipe
   type INTR/ISOC`). Not needed for the synth. The Launchkey's MIDI, HID and mass-storage interfaces all claim
   (6 channels); a synth build can skip the HID and MSC ones with the claim filter to save 3 or 4.

## Caveat on the replug item

The 10 replugs are software: each cycle stops and restarts the USB host, so the hub and both devices disconnect
and re-enumerate, and forced full speed is set again. The hub's power never drops, so it isn't a cable pull.
Because plugging the hub in switches the board off (finding 4), each physical replug also needs a POWER press.
That makes a physical 10-replug run a manual chore; it's worth a few pulls before the synth relies on hot-plug.

## Not covered

- **Notes while the tone plays.** The board handled MIDI and audio claims together (11 channels), but nobody
  played during the soak, so concurrent load is untested. Step 2 (Felucca from the Launchkey) covers it anyway.
- **Why the board dropped off at 05:55.** The log shows nothing before it: no USB disconnect, no reset
  message. It could be power (finding 4), the Mac sleeping its ports, or a crash after 13.5 h. Unknown.

- The same tone on the board's ES8311 speaker (proposal step 1, not in this probe's brief).
- A C-Media CM108/CM119 adapter. The iFi passed instead.
- Latency from key to sound (step 2, with Felucca).

## For step 2 (Felucca into the DAC)

- **44.1 kHz works as asked.** The iFi lists 44.1 kHz and the host paces to its feedback at exactly 44,100
  frames/s, so Felucca's tables need no resampling.
- **The audio callback is pull-driven.** `onAudioOutputRequest` asks for about 44 or 45 frames per 1 ms USB frame
  (one packet each), on the USB client task. Render Felucca in its own task into a ring buffer, and have the
  callback only copy from it. The README warns not to do heavy work in the callback.
- **Set `fifo = ESP_USB_HOST_FIFO_LARGE_PERIODIC_OUT`** in the synth's config, or a DAC with large packets
  won't claim.
