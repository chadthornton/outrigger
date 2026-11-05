# M-VAVE Open firmware: switching to and from it

Status 2026-11-04: research and a read-only layout reader (`lib/fm1_loader/src/app_table.*`). Switching an Open
FM-1 is still blocked (PR #40). Guarded work: nothing here may write to an FM-1 until the hardware probe below
passes. Chad's decisions are at the end.

Sources:
- M-VAVE's FM1 Open, GPL-3.0 app with a binary SDK: github.com/m-vave/fm1_open (V14). Read: `sdk/Package.cs` (the
  layout writer), `sdk/WebUpdate.js` (the Workbench updater), `app/instrument.h`, `app/project.c`, `app/samples.c`.
- The Workbench's release package, sha256 `7e7b6d26…0fd6`, identity `FM-1_8000014` (`tests/fixtures/open-v14.fwsc`).

## Does Outrigger's key reach the ROM bootloader? Yes (probe, 2026-11-05)

**Probe result.** FM-B on Official Open V14 (installed by Chad with the Workbench, force flash from Felucca). Small-V2
ran `fb3dc0d` (BENCH, the backup-only bypass). The Backup job sent the 7D key, read the whole flash and finished OK
(`08d4edf4e6f63ad5-b1`). FM-B came back on Open (3654:7756). The unit ID is FM-B's usual one, so the Workbench
install left 0xE9000 alone.

The dump (`08d4edf4e6f63ad5-b1`, read from Small-V2's store over USB, sha256 `2ca47bae…`):
- 0x0000–0x72FFF is the Open package's flash.bin byte for byte, head included: the Workbench writes from address
  0, so a Workbench-installed Open FM-1 has Open's head. The mixed-head pairs remain only for Outrigger's own
  installs.
- VM 0x73000: one sector formatted (JieLi `55 AA AA 55`), the rest blank. BTIF 0x7B000: blank.
- No Open project or settings records yet (no `FMP6`/`FMS6` magic): 0x7C000–0x8CFFF still holds the previous
  firmware's app bytes, which Open ignores.
- A built-in sample bank was written at install: header `FM1S` at 0x8D000, 76,436 bytes, so it runs to 0x9FA94.
  It covers 0x93000 (stock's factory sector), and Felucca's 0x94000–0x9FFFF. Installing Open with the Workbench
  already overwrites 0x93000.
- 0xE9000 unchanged.
During the job Small-V2 was off Wi-Fi for about 25 minutes, yet its uptime afterwards dated its boot before the
job. Small-V1 showed the same Wi-Fi stall on a build with master 7945bac, so it's probably not Open-specific.

What led here:

- Outrigger's identity query (`F0 00 32 45 00 00 00 40 7F F7`) is M-VAVE's update-protocol command 17. The
  Workbench sends it to Open and gets 27 bytes of ASCII back, so Open answers it. `FM-1_8000014` passes
  `valid_identity()` (test: `test_open_identity_reply`).
- The Open engine (`sdk/lib/libfm1_engine.a`) holds the exact key `F0 22 24 35 7D F7` once, beside
  `usb_update_mode`. Nobody has seen Open act on it.
- The Workbench uses a different path: command 49 (capability: family 7, build hash), command 50 (arm), then
  `F0 22 24 35 7F F7` starts M-VAVE's own loader, which pulls the package in 512-byte blocks (command 48). If the
  7D key fails, Outrigger needs a client for that protocol instead.

## Two flash layouts

A firmware's identity and USB ID don't tell the layout apart: `FM-1_8xxxxxx` firmwares come in both. The loader
reads the image's own file table at 0x4000 (`read_app_table`, `layout_of`).

| Range | V15 layout (stock, Felucca, Skiff, SLOOP …) | Open layout (Open V14, Big Screen, amenity, clatter) |
|---|---|---|
| 0x0000–0x3FFF | boot head | boot head, with different clock and supply settings (below) |
| 0x4000–image end | app | app; the image ends at 0x73000 (Open, clatter) or 0x72000 (amenity) |
| image end–0x7AFFF | app | JieLi VM: MIDI and system preferences |
| 0x7B000 | app | Bluetooth data (BTIF) |
| 0x7C000–0x8AFFF | app | 15 project slots |
| 0x8B000, 0x8C000 | app | settings; project staging and tuning journal |
| 0x8D000–0xF9FFF | app tail, VM (0x93000–0xE8FFF), BTIF 0xE9000 (our unit ID), resources from 0xEA000 | sample bank, header first |
| 0xFA000–0xFDFFF | stock resources to 0xFBFFF, then Felucca globals | DX7 voice library, four banks |
| 0xFE000 | Felucca globals | tuning |
| 0xFF000 | key_mac | key_mac |

What collides with `layout.h`'s rules:
- Installing any V15 firmware writes 0x4000–0x92FFF, over Open's preferences, Bluetooth data, projects, settings and
  sample header.
- An Open sample bank erases only the sectors it fills. Over 24 KB it reaches 0x93000 (stock's factory sector).
  Over 376,832 bytes (about 16.5 of its 19.5 seconds) it reaches 0xE9000: the board then sees a different unit ID,
  and stock loses its Bluetooth data.
- Samples and the voice library cover stock's resources (0xEA000–0xFBFFF). The Workbench's return to V15 writes four
  factory voice banks back over SysEx afterwards; Outrigger has no way to.
- A clean slate erases 0xFC000–0xFEFFF: Open's voice banks C and D, and its tuning.

Boot heads (decoded `isd_config`): stock's sets SYS_CLK 360 MHz and VLVD; Open's sets 320 MHz, VDDIO, DVDD and
DCDC14. Outrigger never writes the head, so it would make two untested pairs: a V15 app on Open's head, and Open on
stock's.

## Test plan

Mac, no FM-1 (done in this branch): `make test` runs `test_fm1_apptable`:
- the four V15 fixtures read as V15, Open V14 as Open with the map above;
- a whole-flash read gives the same answer as the package;
- a blank flash, a short read, and a one-bit flip in the VM record all give no table;
- a table that fits neither layout is Unknown;
- Open's identity reply parses.

Mac, still to write (decisions below): Open-layout packages in the store, the app compare stopping at the
image end, a `Family::Open` shelf and its guard ranges, install-Open erasing up to 0x92FFF, the Open→stock warning,
and the Python side (`tools/fm1lib.py`) with the interop test.

Hardware, one card each, on hub B (Small-V2) with FM-B:
1. **Probe (done 2026-11-05: the key works).** Chad puts FM-B on Open with the Workbench (force flash). A
   BENCH-only Backup job sends the key and reads the whole flash; nothing is written. It answers whether the key
   works, whether the Workbench wrote Open's head, where VM starts, and whether 0xE9000 is still stock's sector.
2. **Switch loop, after the code.** Open → Felucca → Open → stock, checking that each boots and plays (including
   both mixed-head pairs) and that Open's projects come back.

## Decisions (Chad, 2026-11-04, in chat)

1. **Switching away from Open:** projects, settings and MIDI preferences come back (a `Family::Open` shelf, image end
   to 0x8CFFF). Samples and the voice library don't; the user reloads them in the Workbench.
2. **Open → stock:** warn, then allow. The warning says stock's factory voices need the Workbench's restore step
   (the Skiff 2.x warning's pattern).
3. **A sample bank that reaches 0xE9000:** warn when it happens. When an Open FM-1's unit ID matches no known unit,
   Outrigger says its samples may have overwritten the sector it recognises FM-1s by, and that older backups are
   still listed under the previous FM-1, instead of silently treating it as new.
