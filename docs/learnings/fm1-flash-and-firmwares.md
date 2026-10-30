# The FM-1's flash, firmwares and save states

[Index](index.md) · previous: [The FM-1 on USB and in ROM boot](fm1-usb-and-rom-boot.md) · next: [Porting to other ESP32 boards](porting.md)

## The flash map

The FM-1 has 1 MiB of SPI NOR flash: 256 sectors of 4 KiB. The constants live in `lib/fm1_loader/src/layout.h`; the
Felucca-family data ranges come from Felucca's own source (`firmware/src/storage.c`, `firmware/loader/ldr_core.c`).

| Range | Holds | The loader may |
|---|---|---|
| `0x00000–0x03FFF` | Boot head | **Never write.** A bad boot head can't be repaired over USB. |
| `0x04000–0x92FFF` | App area, 143 sectors | Write: this is all an install writes. |
| `0x93000–0xE8FFF` | Stock V15's settings store (JieLi's "VM"), with factory data at `0x93000` and an update record at `0xE8F00` | Erase only through an explicit "erase settings" choice. |
| `0x97000–0x9EFFF` | Felucca family: projects and songs | Restore from a save of the same unit and firmware. |
| `0xA0000–0xDBFFF` | Felucca family: user sample slots | Same. |
| `0xDC000–0xDFFFF` | Felucca family: user preset banks | Same. |
| `0xE0000–0xE4FFF` | Felucca family: update staging (update loader and record) | **Never save or restore.** A stale update record could start an update. |
| `0xE5000–0xE6FFF` | Felucca family: autosave A/B | Restore. |
| `0xE7000–0xE7FFF` | Felucca family: voice bank | Restore. |
| `0xE9000–0xFBFFF` | Per-unit Bluetooth data (`0xE9000`) and factory voices | **Never write.** `0xE9000` is also what names the unit. |
| `0xFC000–0xFEFFF` | Settings and globals | Restore, or erase through "erase settings". |
| `0xFF000–0xFFFFF` | Erased at the factory | Never written. |

All of this goes through one gate, `guard()` in `layout.cpp`, with three rights: app (installs), save restore, and
settings erase. A request that only straddles a forbidden edge is refused too; the tests check every edge.

**Measured at checkpoint 1 (2026-10-27):** after a read-only backup, the boot head and the
`0xE9000–0xFBFFF` area matched the 2026-10-25 dump of the same unit byte for byte. After checkpoint 3's installs
(2026-10-30) they still did.

**Stock V15's data map is unknown.** Its store is the whole VM range, but which sectors hold what isn't mapped. So
stock save states stay disabled, and a switch to stock keeps the data in place behind the layout warning. The loader
never erases `0x93000`. Mapping it needs a stock unit's dumps before and after changing settings.

## Packages

A firmware package is a `.fwsc` file wrapping a JieLi UFW image (`lib/fm1_loader/src/package.*`). A package passes
only if:
- it parses;
- its UFW header and entry-list CRC-16s are correct;
- its `flash.bin` reaches `0x93000`;
- its identity string is a known `FM-1_NNN`;
- it matches the SHA-256 recorded when it was added to the board's library.

The app image is encrypted with a scramble that depends on the flash address. Two consequences: the same code at a
shifted address shows up as different bytes, and zlib shrinks packages only to 0.96×.

Identities the tests check (`fm1lib.py selftest` in `make test`): Skiff 1.4 is `FM-1_714`, SLOOP 2.8.1 is
`FM-1_900`, stock V15 is `FM-1_015`.

## Firmware families

| Family | Members here | Data layout |
|---|---|---|
| Stock | V15 (Apache-2.0, from m-vave/fm1_open) | The VM range, unmapped. |
| Felucca 1.x | Felucca 1.6.1; Skiff, built on Felucca 1.6.1 | The Felucca table above. |
| Felucca 0.9 | SLOOP 2.8.1, built on Felucca 0.9 | The Felucca table, plus possibly data outside it (**open**). |

**Why the family matters:** one family's settings can crash another's firmware. Skiff over Felucca 0.9 data is a
known brick pattern (recorded by Chad on 2026-10-27). That's why checkpoint 3 used Felucca 1.6.1, Skiff's own base,
instead of SLOOP: SLOOP waits until store dumps (`tools/store_dump.py`) show where it keeps data outside the Felucca
table.

**The layout warning** (safety rule 6) follows the Unbricker's repair windows. A family change warns that old
settings can crash the new firmware, and offers the settings erase. A family change that keeps the old data (a
switch to stock) needs the user to accept the layout warning too (`c55a581`).

## Save states

A save state is one firmware's user data, taken from one unit. It's keyed by unit, firmware identity and app hash,
and it holds only the non-blank 4 KiB sectors of that family's data ranges with their addresses. A typical save is
tens of KB; the worst case, with full sample slots, is about 360 KB.

**What an install does with data** (Chad's defaults, decision 0 in the plan):

| Case | Data the new firmware starts with |
|---|---|
| Switching back to a firmware you've saved | The save, restored automatically. |
| Updating within a firmware | Kept in place, as the official installers do. |
| Switching families | A fresh start; the old firmware's saves stay in the store. |

Before any install writes, the current firmware's data is autosaved.

**Measured, 2026-10-30, build `fc72748`:**
- **Felucca 1.6.1 → Skiff 1.4:** the preview read "Writes 143 of 143 app sectors / Restores its saves (6
  sectors)". The install verified all 149 sectors, autosaved Felucca's data first, and cleared its pending record.
- **Marked round trip:** Chad set Skiff's tempo to 137 and changed a pattern step, backed up, then went Skiff →
  Felucca → Skiff. Both changes came back. The restored save was captured from that backup, and the install read
  back each of its 6 sectors.

## The catalog, and what fits on the board

[The Firmware Hub](https://thefirmwarehub.com/)'s `catalog.json` listed 103 packages on 2026-10-26. All of them were
downloaded, each matched its catalog SHA-256, and `tools/catalog_dedupe.py` compared their app areas sector by sector:

- **Size:** each app area is 143 sectors (0.56 MB). All 103 together are 57.3 MB of non-blank sectors.
- **Sharing:** about 67 sectors are shared widely, the most common by 93 packages. Stored once, the total drops to
  40.7 MB (1.41×). The median package needs 472 KB of its own.
- **Design consequence:** one content-addressed sector store for packages, backups and saves
  (`lib/fm1_loader/src/store.*`). Each 4 KiB sector is stored once under its hash. A unit's app area usually equals
  a library package, so its backup costs almost nothing extra.
- **Budget:** a 12.1 MB `store` partition was estimated to hold about 24 packages alongside backups and saves. That
  estimate came before the overhead below was measured.
- **Real overhead is higher.** A file per 4 KiB sector costs LittleFS a metadata block too. The first backup used
  about 1.7 MB; 4 packages took 4.3 MB, about 2.6× their size. Packing sectors into larger files is a follow-up.
- **Possible win, unmeasured:** decrypting before sharing sectors might save far more than 1.41×, because the
  scramble hides identical code at shifted addresses.

**Licenses** (from the catalog, 2026-10-26): 93 packages are GPL-3.0, 8 are MPL-2.0, and stock V15 is Apache-2.0.
Stock V16, Baud Girl and Groove OS aren't redistributable: the board can install a copy you downloaded yourself, but
the repo never carries them.

**Trust:** a matching SHA-256 proves a download matches the catalog, not that the catalog is trustworthy. The
loader never touches the boot head, so a bad app can always be overwritten through ROM boot, unless it also ignores
the ROM-boot key. That case needs the Transporter.
