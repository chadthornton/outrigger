# The FM-1 on USB and in ROM boot

[Index](index.md) · previous: [What broke in the USB host](usb-host-failures.md) · next: [The FM-1's flash, firmwares and save states](fm1-flash-and-firmwares.md)

The M-VAVE FM-1 is a small FM synth built on a JieLi chip (the wl82 family) with 1 MiB of SPI NOR flash. It shows
up on USB as several different devices depending on what it's running. This page covers what each face looks like
to a host, and how the loader gets from the playing synth to the ROM bootloader through a hub.

## Its USB identities

| State | VID:PID | Product name | Where the code knows it |
|---|---|---|---|
| Stock firmware (V15) | `4C4A:C755` | | `midi_core::device_role()` |
| Felucca-family firmware (Felucca, Skiff, SLOOP) | `1209:0001` | `Felucca` | `midi_core::device_role()` |
| Stock update mode | `4D4A:4155` | `ota-FM-1` | plan, "Alternative considered" |
| Felucca update mode | `1209:0002` | `Felucca Update` | plan, "Alternative considered" |
| JieLi ROM bootloader | `4C4A:8057` | `WL80UBOOT1.00` (as logged by the board) | `platform::classify()` |

**Skiff calls itself "Felucca".** Skiff is built on Felucca and keeps its USB identity, so `1209:0001 Felucca` doesn't
tell Skiff from Felucca. Only the identity string the firmware reports (Skiff 1.4 is `FM-1_714`), or a backup of the
flash, tells them apart. Checkpoint 3 confirmed this on 2026-10-30.

**Skiff's interfaces** (from Skiff's source, recorded in `docs/audio-recording-spike.md`): audio control, MIDI
streaming, audio streaming (UAC1, 16-bit stereo, 44.1 or 48 kHz, iso IN `0x84`, 196-byte packets), then a CDC
serial pair. The CDC pair is why Skiff needs CDC claiming off to fit the
[8-channel budget](usb-host-esp32s3.md#the-8-channel-budget).

**Two FM-1s look identical on USB.** They have the same VID:PID and name, and no USB serial number. Only the flash
tells them apart: the loader names a unit by a hash of its per-unit Bluetooth sector at `0xE9000`, which no
installer writes.

## Two ways to install firmware

1. **The update protocol over USB-MIDI SysEx.** Every normal installer uses it: stock M-UPGRADE, the
   Felucca-family installers, The Firmware Hub. The FM-1 re-enumerates in update mode, asks for ranges of the
   package, stages the update, and applies it after a restart. An interruption between staging and applying is what
   bricked Chad's unit, which the XIAO Transporter and Quixotic7's Unbricker recovered through ROM boot on
   2026-10-25. The protocol is documented in
   [AL-255/FM-1-RE](https://github.com/AL-255/FM-1-RE/blob/main/docs/io/11-ota-protocol.md) and
   [Felucca's `tools/fm1_install.py`](https://github.com/hugelton/Felucca/blob/main/tools/fm1_install.py).
2. **The JieLi ROM bootloader.** It's a USB mass-storage device that takes vendor SCSI commands. After a small loader
   is uploaded to RAM, the host writes the flash directly and can read every byte back. An interrupted write is
   fixed by running it again, as long as the ROM bootloader can still be reached.

The loader uses path 2. Path 1 writes nothing the host can verify and can't take a backup, and its apply step is
the one that bricked a unit (`docs/firmware-loader-plan.md`, "Why ROM boot").

## Getting into ROM boot over MIDI

A working FM-1 enters ROM boot when it gets a particular SysEx message (the ROM-boot key). It's ordinary USB MIDI,
so it passes through a hub. The other way in, the USB_KEY pulses at power-on, can't pass through a hub;
that's why reviving a dead FM-1 stays the Transporter's job.

The sequence, ported from the Unbricker's `softkey` (`lib/fm1_loader/src/softkey.*`, tested in
`tests/test_fm1_softkey.cpp`):
1. **Ask who it is.** An identity query, up to 3 tries, 1 s each. A working FM-1 answers `FM-1_NNN`.
2. **Refuse anything else:** no answer, an `ota-…` identity (update mode), or a string that isn't `FM-1_NNN`.
3. **Send the key:** one 8-byte USB-MIDI transfer, sent 3 times 20 ms apart.
4. **Wait** up to 15 s for `4C4A:8057` (the Unbricker's own timeout).

**Measured at checkpoint 1 (2026-10-27):** Skiff accepts the key. It answered the identity query as `FM-1_714`, took
the key and entered ROM boot. Before that it was only reported for Felucca-family firmware.

**Who may send it:** only the Loader, only in Load mode, only when the broker says the Loader owns the FM-1's MIDI.
The Router drops every SysEx in every mode, so a controller can never send it.

## The ROM-boot device

Once in ROM boot the FM-1 enumerates as `4C4A:8057`, a mass-storage device. SCSI inquiry reads vendor `WL82`, product
`UBOOT1.00`; the loader requires both before anything else (`installer.cpp`). Every command after that is a vendor
CDB sent through the patched library's `mscRawCommand()`.

**Command framing** (`lib/fm1_loader/src/rom.*`, layouts from
[jl-uboot-tool](https://github.com/kagaimiq/jl-uboot-tool)'s `jltech/uboot.py`):
- Inquiry is a 6-byte CDB. Vendor CDBs are 16 bytes, padded with `0xFF`.
- Every command execution is a 16-byte data-in, including erase, jump and reset.
- A flash read (`FD05`) must arrive whole; only the short 16-byte responses and inquiry may come back short
  (`4f53738`).

**The session, in order:**
1. **Upload the loader.** `wl82loader.bin` goes up in 512-byte blocks to `0x1C02000`, then a jump with argument 1
   (SPI NOR). The wl82 ROM unscrambles it itself, so jl-uboot-tool's host-side scramble condition is false for this
   chip; both branches are ported and tested.
2. **Check the chip.** The flash ID must be `0x856014`, and the chip key the loader reports must match the wl82's.
3. **Read, erase and write** in 4 KiB sectors. There's no chip erase or block erase in the client at all.
4. **Reset** with `0xFC0C`, argument 1 (jl-uboot-tool's `reset`). `0xFC02` is the older V1 loader's reset; the wl82
   runs the V2 loader. The Unbricker never resets; it has the user power-cycle. Checkpoint 1 confirmed `0xFC0C`
   restarts the FM-1 into its firmware.

Left out on purpose: the Unbricker reads 512 bytes of RAM (`FD07`) before the upload, for crash diagnosis. The
loader doesn't.

**What the library sends on attach:** nothing beyond enumeration. EspUsbHost's mass-storage block APIs would probe
the device (GET_MAX_LUN, inquiry, capacity), but the loader never calls them. Checkpoint 1 showed the ROM device
took the loader, the IDs and two full 1 MiB reads this way.

## `wl82loader.bin`

- **What it is:** JieLi's own USB loader for the wl82, distributed inside jl-uboot-tool. Its license is unclear, so
  the project treats it as not redistributable.
- **Where it lives:** on the board's store only. `make fixtures` fetches it for the Mac tests, pinned by SHA-256 and
  gitignored. `make store-image` puts it in the store's first image.
- **Where it never goes:** git, a release, a CI artifact, or the installer site. `tools/build_site.py` refuses a
  file with its name or hash, or containing any 1 KiB piece of it, and deletes the staged firmware if it finds one.
- **Checked before the key:** without a loader nothing can reset the FM-1 out of ROM boot, so the loader blob is
  verified before the ROM-boot key is sent (`c55a581`).

## The ~8.5 s self-restart

**Observed** (checkpoint 3, leg 2, 2026-10-29): twice, the FM-1 left ROM boot on its own about 8.5 s after the
loader started. Both times the install was still in its two full backup reads and planning, before the first erase.
A resume, which reached its first erase 3.3 s in, then wrote for 9 s without a stop.

**Hypothesis:** the loader has a watchdog that erases and writes feed and reads don't. **Unconfirmed.** The test
that would confirm it: time a backup that stops reading for 9 s.

The soak logs fit it without testing it: a backup's two 1 MiB reads end about 7.6 s after identification (for
example step 7 at 11.57 s, step 12 at 19.13 s board time), just under 8.5 s, and backups finish.

**What the design does about it** (`d860144`):
- The install plans from the unit's stored backup before the key, so in ROM boot it reads the flash once, not twice.
- `LoaderTooSlow` refuses the first erase if the loader has run for more than 6 s (`kLoaderBudgetMs`).
- **Measured:** in the marked round trip on 2026-10-30, the first erase came 2.1 s after the loader started.

## What's still unknown about the ROM device

- What inquiry reports once the loader is running. The resume path asks the loader directly (`online_device`,
  `0xFC0A`) instead of trusting inquiry (`75fa17d`).
- The loader's USB buffer size (`0xFC14`). The mock says 512.

See [open questions](open-questions.md).
