# Boards: what can host, what to buy

Outrigger builds for every board with a profile in `firmware/boards.json`: today the Waveshare ESP32-S3-Touch-LCD-1.69
and the Waveshare ESP32-S3-Touch-LCD-3.5. The installer on outrigger.music asks which one you have.

## Will my board work?

It needs all four:

1. **An ESP32-S3.** The ESP32-S2 and P4 can host USB too, but Outrigger has no S2 or P4 build yet (P4 notes below). A
   plain ESP32, C3 or C6 can't host USB at all, so the original "Cheap Yellow Display" (ESP32-2432S028) never will.
2. **A USB port wired for host mode:** the USB-C goes straight to the chip's own USB pins (GPIO19/20 on the S3), not
   through a converter chip. "The one test" below says how to tell.
3. **16 MB of flash and 8 MB of PSRAM.** The firmware, its recovery copy and the backup store with the three starter
   firmwares fill a 16 MB layout (`firmware/waveshare_169/partitions.csv`), and the loader works in PSRAM. Listings
   say "N16R8". Smaller boards could run the MIDI hub alone, but no build does that yet.
4. **A screen Outrigger can drive.** Today that's an SPI ST7789 or ST7796 panel up to 480×480, with CST816 or FT6336
   touch, optionally behind a TCA9554 I/O expander. Another chip means a few lines of driver code in
   `firmware/waveshare_169/board.cpp` as well as a profile.

A board that passes 1 to 3 but has a different screen is still a good candidate: send us its schematic.

## Adding a board

A board is a profile, not new code:

1. **Find the primary sources:** the maker's schematic and pin map, or its example code. Note every pin the screen,
   touch and backlight use, the I2C address of each chip, and anything on the board that makes noise if left floating
   (a buzzer, a speaker amplifier's enable).
2. **Add a profile to `firmware/boards.json`,** copying the nearest one. The `_about` line explains the fields. Pins
   are GPIO numbers, `"exioN"` for pin N of the I/O expander, or `null`. `hold_low` lists the noisy pins; say why in
   `hold_low_why`. Pick the next free `tag_id` and a `site_id` (the folder on the site).
3. **Run `python3 -I tools/boards.py check`.** It refuses pins on the USB lines (19/20), on the flash lines (26–32), on
   octal PSRAM (33–37), a pin with two jobs, and memory below 16 MB flash and 8 MB PSRAM.
4. **Run `make ui-shots`.** Every screen renders at the new resolution; it fails on text that doesn't fit.
5. **Run `make build BOARD=<id> APPS=hub-loader`,** then flash a board and check the screen is upright, taps land under
   the finger, and the FM-1 plays through it. Add the board's row below.

`make site` builds every board and the installer lists them all.

## Research

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
| Waveshare ESP32-S3-Touch-LCD-1.69 | 240×280 rounded, touch (CST816T) | **Confirmed**, in use | 16 MB flash, no TF | Profile `waveshare_169`. The buzzer on GPIO42 is held low. |
| Waveshare ESP32-S3-LCD-1.47 | 172×320, **no touch** (BOOT/RST only) | Likely: its USB plug is USB-A male; the wiki doesn't state the wiring | TF slot, 4-bit SDMMC (GPIO14–18, 21) | Needs a USB-A-female-to-USB-C-female adapter to reach the hub's upstream cable. |
| Waveshare ESP32-S3-Touch-AMOLED-1.8 | 368×448 AMOLED, touch | Likely: the docs call its one USB-C the "ESP32-S3 USB port", the same wording as the 1.69" | TF slot | Speaker, mic, ES8311 codec, AXP2101. **V1** uses SH8601/FT3168, **V2** (since May 2026) CO5300/CST820, so the profile must match the unit received. |
| Waveshare ESP32-S3-Touch-LCD-3.5 | 320×480, ST7796 (SPI), FT6336 touch | **Confirmed** (schematic): the V1 and V2 schematics show IO19/IO20 at the USB-C through 22 Ω, no converter chip | 16 MB flash, 8 MB PSRAM; TF slot (V1 SPI; V2 SDMMC plus card-detect) | Profile `waveshare_35`, details below. Camera port. The same display and touch chips as the P4 3.5". Not the **3.5B** (AXS15231B over QSPI), a different board. |
| AITRIP ESP32-S3 4.0" 480×480 (UPC 687117784421, ASIN B0GL1PHKYG) | 480×480 RGB parallel | **Likely not**: "one-click download" and one USB port point to a converter chip | TF slot | Looks like the Sunton/Guition ESP32-4848S040 family (unconfirmed). Could serve later as a Wi-Fi control panel. |
| Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3 | 480×800 MIPI-DSI (ST7701), GT911 | Unknown: "Type-C ports" | TF slot, 32 MB flash, 32 MB PSRAM | See "ESP32-P4" below. |
| Waveshare ESP32-P4-WIFI6 3.5" Smart Vision | 320×480 ST7796 (SPI), FT6336 | Unknown: "Type-C ports" | TF slot, 16 MB flash, 32 MB PSRAM | OV5647 camera, mic, speaker, AXP2101. **The better P4 experiment**: same display and touch as the S3 3.5", so a spike isolates the USB question. |

## Waveshare ESP32-S3-Touch-LCD-3.5

From Waveshare's repo (`waveshareteam/ESP32-S3-Touch-LCD-3.5`): the V1 and V2 schematics' pin tables, and the Arduino
examples `08_gfx_helloworld` and `11_lvgl_arduino_v8`. Board-3 (MAC 44:BD:8D:61:2B:28) read as ESP32-S3 with 8 MB
embedded PSRAM and 16 MB flash.

- **Display:** ST7796, 320×480 IPS, SPI on SCK 5, MOSI 1, MISO 2, DC 3. CS is tied low. Backlight on GPIO6. The
  panel's reset is pin 1 of the TCA9554 expander (EXIO1), so the driver pulses it over I2C.
- **Touch:** FT6336 at 0x38 on I2C SDA 8, SCL 7, shared with the expander (0x20), the AXP2101 power chip, the
  ES8311 codec, the RTC and the IMU. Its interrupt is EXIO2; no reset line.
- **No buzzer.** EXIO7 enables the NS4150B speaker amplifier. The expander starts with every pin an input pulled high,
  which turns the amplifier on, so the profile holds EXIO7 low.
- **USB:** one USB-C, straight to GPIO19/20. Power comes up the same cable from the hub (VBUS into the AXP2101).
- **V1 and V2** use the same pins for all of the above; they differ in the TF slot's wiring and IO4.
- The camera connector takes GPIO17, 18, 21 and 38–48; Outrigger doesn't use it.

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
