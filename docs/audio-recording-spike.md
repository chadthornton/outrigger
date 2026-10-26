# Spike: recording the FM-1's USB audio on the Waveshare host

Branch `spike/audio`, throwaway. Question: can the Waveshare ESP32-S3 USB host capture the FM-1's USB audio and
store it?

## Verdict

Pending measurement (see [Measurements](#measurements)).

## What the FM-1 sends (research, 2026-10-26)

Source: Skiff's `firmware/src/usb.c` and `firmware/src/felucca.c` (github.com/illuminus5/skiff, `b03d98a`).
Felucca, which Skiff is built on, has the same code.

- **Skiff keeps the audio input.** `FELUCCA_UAC` defaults to 1: a USB audio input that carries the master output.
- **Format:** UAC1, class compliant, PCM 16-bit stereo. One alternate setting (alt 1) offers two discrete rates,
  44.1 kHz and 48 kHz, chosen by the host with the UAC1 sampling-frequency `SET_CUR`. The synth engines always run
  at 44.1 kHz. At 48 kHz Skiff resamples with a polyphase FIR, so **44.1 kHz is the native, bit-exact rate**.
- **Endpoint:** EP `0x84`, isochronous **asynchronous** IN, 1 ms interval, wMaxPacketSize 196 (49 frames × 4 bytes).
  At 44.1 kHz each packet carries 44 or 45 frames (176 or 180 bytes), so the stream is 176.4 kB/s.
- **Start-up:** alt 0 has no endpoint. After the host selects alt 1, Skiff sends silence packets until its ring is
  primed, so a short stretch of leading silence is expected, not a dropout.
- **Interfaces** (with the serial console, as on Chad's unit, which shows a CDC port): IF0 audio control, IF1 MIDI
  streaming, IF2 audio streaming, then the CDC pair. `bcdDevice` of that build should read `0x0329`.
- **Level:** the USB feed follows the MASTER knob unless MENU › USB LEVEL is FIXED. SPEAKER EQ also shapes the USB
  feed, so record with it on FLAT.
- Stock V15 firmware (`4C4A:C755`) also exposes USB audio, but Chad's unit runs Skiff, so only Skiff matters here.

## What the host library offers

Vendored EspUsbHost 2.10.1 (`lib/EspUsbHost`), marked experimental for audio:

- `getAudioStreams()` lists streams (rates, bits, channels, maxPacketSize, `startable`).
- `audioInputStart(channels, bits, rate)` selects alt 1, sends the rate request and queues
  4 transfers × 8 iso packets (about 32 ms of buffering). `audioInputStop()` returns to alt 0.
- `onAudioData` fires **once per iso packet** on the USB client task, with the payload. Packets with an error status
  or zero length are **skipped silently**, so dropouts must be found by counting: frames received vs. elapsed time
  × 44100, and packets vs. elapsed milliseconds.
- Full speed only: 196-byte packets fit easily (the cap is 1023).

## Channel budget

The ESP32-S3 host controller has 8 channels. ESP-IDF allocates one per open pipe at claim time, not when data flows.

- **The audio endpoint costs a channel as soon as the FM-1 enumerates.** The library claims any audio streaming
  interface that has endpoints during enumeration, so on Skiff the FM-1 costs 4 (EP0 + MIDI bulk IN + MIDI bulk OUT
  + iso IN), not the 3 the README counts.
- **Open question:** the README measured hub + FM-1 + Launchkey working at 8/8. With the audio pipe that should be
  9, so either the audio claim failed silently in that rig (Launchkey enumerated first), or the unit's build has no
  audio. The spike firmware shows each interface's claim result to settle this.

| Mode | Channels | Fits? |
|---|---|---|
| Hub + FM-1 (MIDI + audio), capture to PSRAM | 2 + 4 = 6 | yes |
| Hub + FM-1 + Launchkey, capture while playing | 2 + 4 + 3 = 9 | no, unless the FM-1's MIDI IN endpoint is left unclaimed (library patch) → 8 |
| Hub + FM-1 (MIDI + audio) + USB stick | 2 + 4 + 3 = 9 | no |
| Hub + FM-1 (audio only, MIDI unclaimed) + USB stick | 2 + 2 + 3 = 7 | yes, needs a library patch to skip the MIDI interface |

**What the audio interface costs on its own: 1 channel.** Skiff's audio streaming alt 1 declares one endpoint
(`bNumEndpoints` 1, EP `0x84` iso IN). There is no feedback endpoint: explicit feedback belongs to asynchronous
*playback* (OUT) streams, and the FM-1 has no USB audio output. The audio control interface (IF0) has no
endpoints and is driven over EP0. So an FM-1 used for audio only costs EP0 + iso IN = **2**.

**Recording while playing, with the FM-1's MIDI moved to Bluetooth:** hub 2 + Launchkey 3 + FM-1 audio 2 =
**7 of 8**. That needs EspUsbHost to leave the FM-1's MIDI streaming interface unclaimed, and today it can't:
the enumeration code (`EspUsbHost.cpp`, the claim decision around line 9430) claims the first MIDI streaming
interface of every device unconditionally, and there is no filter hook or config macro to skip it. A vendored
patch is small: a callback such as `bool (*claimFilter)(const EspUsbHostDeviceInfo &, uint8_t ifClass,
uint8_t ifSubClass)` checked next to `isMidiInterface`, or a compile-time VID:PID to skip, in the same style as
the existing `ESP_USB_HOST_MAX_SERIAL_PORTS` patch. The same patch makes the USB-stick row below fit.

Storage without the hub's channels: PSRAM (8 MB, about 47 s at 176.4 kB/s), or an SD card on a board that has a TF
slot (the 1.47" Waveshare; the 1.69" has none). Flash is too small and slow to be a recorder.

## Plan for the test firmware

`firmware/audio_spike/`, its own sketch, built with `make spike` and flashed with `make flash-spike`. It never sends
MIDI to anything (no `midiSend` call in the sketch).

1. Show the enumeration facts on screen: FM-1 VID:PID and `bcdDevice`, each interface's claim result, the audio
   streams, and the channel estimate.
2. Start capture at 44.1 kHz stereo 16-bit, explicitly (the library's default scoring would pick 48 kHz and turn
   on Skiff's resampler).
3. Capture rolling 10 s windows into PSRAM and show per window: frames received vs. expected, packets vs. ms,
   peak and RMS level (a meter that moves when notes are played), and the longest gap between packets.
4. Alternate windows with the screen frozen and updating, to see whether LVGL drawing starves the USB task.

## Measurements

### 1. Hub + FM-1 + Launchkey (2026-10-26, photo of the spike screen)

```
AUDIO SPIKE  ch 8/8
05E3:0610 ch2 -      USB2.1 Hub
1209:0001 ch3 CC-F-- Felucca
1235:0142 ch3 CCFF   Launchkey
FM-1 bcd 0329  Felucca
no audio streams
```

- `bcdDevice 0x0329` confirms the unit's Skiff build has the audio input (UAC + 48 kHz + AS-first + CDC, per
  Skiff's `BCD_LO`).
- The FM-1's interface entries read `C C - F - -`: audio control and MIDI claimed, the audio streaming alt 1
  **tried and failed**, the CDC pair skipped (claiming disabled). Its audio pipe never opened, so it costs 3
  channels here and the library lists no audio streams.
- **This settles the README's 8/8 anomaly:** with the Launchkey in, the channels ran out and the FM-1's audio
  claim failed silently. The rig only worked because the audio pipe was the one that lost.
- The Launchkey also shows two failed claims, but it holds 3 channels (EP0 + MIDI in/out), as before.
- The spike firmware does no routing, so the Launchkey cannot play the FM-1 while it runs.
