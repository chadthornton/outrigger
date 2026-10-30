# Boards: what can host, what to buy

Research from 2026-10-27, done before Chad bought more boards. "Confirmed" means a primary source: a schematic, or a
datasheet from the maker. Re-check before relying on any entry marked "likely".

## The one test: native USB on the connector

The ESP32-S3 (and P4) can only be a USB **host** through their built-in USB pins (GPIO19/20 on the S3). Some boards
put a USB-to-serial converter chip (CH340, CH343, CP2102) between the USB-C and the chip. Those talk to a computer
fine but can never host.

**Good signs:**
- the port is labelled "ESP32-S3 USB", "native USB" or "OTG";
- there's no converter chip on the board;
- there are two USB-C ports, one of them native;
- there's a published schematic you can check.

**Bad signs:**
- "One-click download" or "auto download";
- CH340/CP2102 as the only USB path.

A TF slot says nothing about hosting.

**Pins:** a 16-bit parallel (RGB) display uses most of the S3's pins. On those boards, check that GPIO19/20 are free.

## Boards checked

| Board | Screen | Host? | Storage | Notes |
|---|---|---|---|---|
| Waveshare ESP32-S3-Touch-LCD-1.69 | 240×280 rounded, touch (CST816T) | **Confirmed**, in use | 16 MB flash, no TF | The current board. |
| Waveshare ESP32-S3-LCD-1.47 | 172×320, **no touch** (BOOT/RST only) | Likely: its USB plug is USB-A male; the wiki doesn't state the wiring | TF slot, 4-bit SDMMC (GPIO14–18, 21) | Needs a USB-A-female-to-USB-C-female adapter to reach the hub's upstream cable. |
| Waveshare ESP32-S3-Touch-AMOLED-1.8 | 368×448 AMOLED, touch | Likely: the docs call its one USB-C the "ESP32-S3 USB port", the same wording as the 1.69" | TF slot | Speaker, mic, ES8311 codec, AXP2101. **V1** uses SH8601/FT3168, **V2** (since May 2026) CO5300/CST820, so the profile must match the unit received. |
| Waveshare ESP32-S3-Touch-LCD-3.5 | 320×480, ST7796 (SPI), FT6336 touch | **Very likely**: the V1 and V2 schematics show no converter chip, with IO19/IO20 at the USB-C | TF slot (V1 SPI; V2 SDMMC plus card-detect) | Camera port. The same display and touch chips as the P4 3.5". |
| AITRIP ESP32-S3 4.0" 480×480 (UPC 687117784421, ASIN B0GL1PHKYG) | 480×480 RGB parallel | **Likely not**: "one-click download" and one USB port point to a converter chip | TF slot | Looks like the Sunton/Guition ESP32-4848S040 family (unconfirmed). Could serve later as a Wi-Fi control panel. |
| Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3 | 480×800 MIPI-DSI (ST7701), GT911 | Unknown: "Type-C ports" | TF slot, 32 MB flash, 32 MB PSRAM | See "ESP32-P4" below. |
| Waveshare ESP32-P4-WIFI6 3.5" Smart Vision | 320×480 ST7796 (SPI), FT6336 | Unknown: "Type-C ports" | TF slot, 16 MB flash, 32 MB PSRAM | OV5647 camera, mic, speaker, AXP2101. **The better P4 experiment**: same display and touch as the S3 3.5", so a spike isolates the USB question. |

## ESP32-P4

- **Upside:** a high-speed USB 2.0 OTG port with **16 host channels**, double the S3's 8. That fits hub, FM-1 MIDI and
  audio, Launchkey and a USB stick together. Waveshare's sibling P4 boards (the 3.4" round) have a separate USB-A
  host port; check whether these do.
- **Risk:** EspUsbHost says hubs are "effectively not usable" on the P4's high-speed port, with P4 validation still
  ongoing. Our whole design uses a hub.
  - **Possible workaround:** `experimentalForceFullSpeed`, unproven.
  - **Other port:** the full-speed port takes hubs but has only 8 channels.
- **Also different:** Wi-Fi and BLE come through an ESP32-C6 co-processor, the CPU is RISC-V, and the display is
  MIPI-DSI on the 4.3".
- **Advice:** S3 boards for the working ecosystem. One P4 for a short spike with a single question: does a hub work
  on its 16-channel port?

## TF cards

- **Size: 16–32 GB SDHC.** These come FAT32, which the ESP32 reads natively. Cards of 64 GB and up come exFAT and need
  extra work. The whole Firmware Hub catalog is about 60 MB; recording is about 10 MB/min.
- **Speed:** class doesn't matter. Packages are about 0.6 MB, backups 1 MB, recording 0.18 MB/s, and the ESP32's card
  interface is slower than any modern card.
- **Brands:** SanDisk Ultra, Samsung EVO Select or Kingston Canvas Select.
- **Where:** buy from the brand's own storefront. Marketplace listings are where fake-capacity cards come from.
- **How the firmware uses it:** the storage tiers in `docs/architecture.md`. The card is the archive; each unit's
  safety backup stays in internal flash.
