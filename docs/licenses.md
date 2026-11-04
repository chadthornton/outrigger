# Licenses

The license review done before the repo went public (2026-10-31).

**Vendored, ported and linked code:**

| Component | Where | License | In the firmware? | Obligation |
|---|---|---|---|---|
| EspUsbHost 2.10.1 | `lib/EspUsbHost/` (vendored, one patch) | MIT, TANAKA Masayuki | yes | notice; LICENSE kept |
| esptool-js 0.7.0 | `site/vendor/` (replaced ESP Web Tools 10.4.0 when `esptool-installer` merged) | Apache-2.0 | no (site) | LICENSE kept |
| Commissioner font | `site/fonts/` (from the site redesign) | SIL OFL 1.1 | no (site) | notice: **added** |
| MvaveFM1Unbricker | ported: `softkey.*`, `package.*`, part of `crypto.*`, `mock_rom.*`, `tools/fm1lib.py`, `tools/catalog_dedupe.py` | MIT, Michael P Jones | yes | notice: **added** (NOTICE plus a header line in each file) |
| jl-uboot-tool | ported: `rom.*`, part of `crypto.*`; `make golden` imports it from a checkout | MIT, Andrey Grigoryev | yes | notice: **added** |
| LVGL 9.6.0 | Arduino library; `lv_conf.h` is its edited template | MIT | yes | notice: **added** |
| Arduino_GFX 1.6.9 | Arduino library | BSD, Adafruit | yes | notice **with the binaries**: **added** (`site/NOTICE.txt`) |
| Arduino ESP32 core 3.3.12 | toolchain | LGPL-2.1 | yes, statically linked | users must be able to relink: met once the repo is public, since anyone can rebuild with `make build-all` |
| ESP-IDF (inside the core) | toolchain | Apache-2.0, components permissive | yes | notice: **added** |
| FM1Kit (`ocean/FM1Kit`, MIT) | nothing ported; reviewed for the crash lead only | | no | none |
| m-bootloader | nothing ported; not referenced anywhere | | no | none |
| JieLi `wl82loader.bin` | never committed; fetched by `make fixtures`, refused by `build_site.py` | unclear (JieLi's) | **must not ship** | already guarded |

**Binaries ship from the site,** so the MIT and BSD notices travel with them: `make site` copies `NOTICE` to
`site/NOTICE.txt`, and the installer page's footer links it.

**One GPL thread.** `lib/fm1_loader/src/package.*` (`.fwsc`/UFW parsing), the CRC and the UFW header cipher are
ports of the Unbricker's `parse_fwsc`, `ufw_flash`, `ota_crc16` and `ota_jl_enc`. The Unbricker labels these as
ports of `firmware/src/ota.c`. That file is in Felucca, a fork of M-VAVE's `fm1_open`, and both are GPL-3.0.
The Unbricker is published as MIT, but code translated from GPL-3.0 may still carry GPL-3.0. The ported part is
small (package.cpp is 107 lines: a standard CRC-16, a key-0xFFFF XOR cipher and a container header layout), and it is mostly
a description of a file format, which copyright protects weakly. The risk is low, not zero.

**No other GPL code** is in the repo. The GPL-3.0 firmwares (Felucca, Skiff, SLOOP) are test downloads only, and the
board fetches packages from the Firmware Hub at the user's request.

**Decision (2026-10-31): the project is GPL-3.0** (`LICENSE`). The reasons:
- It ends the `ota.c` question.
- It matches the FM-1 firmware community this is announced to: M-VAVE's `fm1_open`, Felucca, Skiff and SLOOP are
  all GPL-3.0.
- Every dependency above is compatible with it: MIT, BSD, Apache-2.0 and LGPL-2.1.
- The author owns the copyright in their own code, so the core can later be relicensed the core (USB host, routing, UI kit) under MIT
  for the toolkit pitch, as long as it leaves out the ported `ota.c` lineage.

The alternatives were MIT with the `ota.c` risk noted, or asking Quixotic7 first.
