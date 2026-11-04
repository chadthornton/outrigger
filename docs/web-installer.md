# The web installer

`site/` is a static page that flashes the firmware from Chrome or Edge with its own installer (`site/installer/`) on
[esptool-js](https://github.com/espressif/esptool-js) 0.7.0 (Apache-2.0, vendored in `site/vendor/`, no CDN). Each tab's
install area reads the board, says in plain words what it runs and what stays, then writes, checks and restarts it in
place. There is no erase option.

```
make site     # build every board x app set, then stage each into site/firmware/<board>/<app set>/ with its manifest.json
make serve    # same, then serve on http://localhost:8088 (Web Serial needs https or localhost)
```

- **Four parts, not a merged image.** The page writes bootloader @0x0, partitions @0x8000, boot_app0 @0xe000 and the
  app @0x10000, exactly what `make flash` writes. A merged image from 0x0 would also overwrite NVS (0x9000-0xDFFF), and
  Arduino's `.merged.bin` is padded to 16 MB, so it would wipe every partition. `tools/build_site.py` refuses parts that
  overlap NVS or overflow app0 when staging, and `site/installer/plan.js` checks again before writing: its own list of
  the four offsets (never the manifest's), each part's 4 KiB-rounded end, and the board's current partition table, so an
  install can't move or drop the store. Both run the cases in `tests/installer/tables.json` from `make test`.
- **What the board runs.** Each build carries a 20-byte tag at flash 0x10120 (`lib/platform/src/build_tag.h`): commit,
  dirty flag, app set. The page reads it before installing; `build_site.py` refuses an app without one. Boards running a
  build from before the tag show as "an earlier usb-midi-host build".
- **A store for the loader.** Installing Hub + Loader or Everything onto a board with no store also sets up the store
  `make flash-store` makes, when the board ran other firmware (its partition table has no store, so what's there isn't
  FM-1 backups; factory boards carry Waveshare's demo and its crash dump) or its store region is factory-blank (the
  board's MD5 of the region matches an erased one's). The browser
  downloads `wl82loader.bin` from the pinned jl-uboot-tool commit on GitHub, checks its SHA-256, and copies it into
  `store-template.json` (`tools/store_template.py`, staged beside each loader build's manifest), so this site never hosts
  it. An existing store is never written. Our store partition with data but no readable store stops the install ("Your ESP32 has storage we
  can't read"); a failed download installs everything else and says firmware switching isn't set up.
  `tests/installer/test_store_template.py` mounts the spliced store and reads the loader back.
- **No laptop copy yet.** Saving the store to the laptop from the page waits for the laptop-backups work
  (`docs/firmware-loader-plan.md`, "Laptop backups"): at the 64-byte reads below the 12.1 MB store takes about 8 min in
  the browser (esptool from a Mac: 79 s). `tools/store_dump.py` already turns a raw read of the store partition back
  into 1 MiB FM-1 dumps.
- **Checks.** Each download is checked against the manifest's SHA-256, each written part against the board's MD5 of it.
  Flash mode, frequency and size are kept as built. Afterwards an RTC watchdog reset starts the program.
- **The board's USB port** stalls about 30 s on any single write of 4 KiB or more, so `site/installer/serial.js` sends
  1 KiB at a time. In Chrome and Brave on macOS its longer replies also stop partway (after 616 bytes of a 1 KiB flash
  read, measured 2026-10-31; Node on the same Mac never lost a byte), so reads ask for 64-byte packets, one at a time.
- **Checked on the Waveshare (2026-10-31, installer 2026-10-31.6):** MIDI Hub installed from Chrome from Computer mode and
  from BOOT+RST; both restarted by themselves; the 12.1 MB store read back byte-identical before and after.
- **App sets:** each tab's buttons name an app set (MIDI Hub, Hub + Loader, Everything), each with its own manifest. Hub +
  Loader ships with the Loader's writes on (`WRITES` defaults to 1 for `hub-loader`; `WRITES=0` builds it read-only);
  Everything's loader stays read-only. The settings page shows which set is running next to the commit (`hub`,
  `hub+L`, `all`).
- **Adding a board:** see [boards.md](boards.md), "Adding a board".
- **Licensing:** our code, LVGL and the vendored EspUsbHost (MIT), Arduino_GFX (BSD) and the Arduino ESP32 core
  (LGPL-2.1) ship as binaries; `make site` copies `NOTICE` to `site/NOTICE.txt` and the footer links it. JieLi's `wl82loader.bin` must
  never be in a published build: `build_site.py` refuses a file with its name or SHA-256, or containing a 1 KiB piece of
  it (when `tests/fixtures/wl82loader.bin` or `$WL82_BLOB` is present), and deletes the staged firmware if so.

Publishing the staged site to outrigger.music: [publishing.md](publishing.md).
