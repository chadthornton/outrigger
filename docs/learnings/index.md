# What usb-midi-host learned

A Waveshare ESP32-S3 board, turned into a USB MIDI host with no computer involved, plays an M-VAVE FM-1 from a
Novation Launchkey through a powered hub. It also installs community FM-1 firmwares (Skiff, Felucca, SLOOP) through
the FM-1's JieLi ROM bootloader. These pages record what the project found out on the way. The design docs
(`docs/architecture.md`, `docs/firmware-loader-plan.md`) say what to build; these pages say what happened.

**Status on 2026-10-30.**
- **Routing works:** Launchkey → FM-1 through the hub has been played by hand since 2026-10-26.
- **The loader's real-install checkpoint passed** on build `fc72748`: Skiff → Felucca 1.6.1 → Skiff, and a Skiff
  project survived the round trip.
- **Not shipped:** writes are still off in released builds. An ESP-IDF USB host assert at the FM-1's disconnect
  after the ROM-boot key (1 crash in the last soak's 8 runs) is unexplained. Everything guarded since `d2284ba` is
  unreviewed.

**Who wrote this.** An AI coding session (Claude) wrote these pages from the repo's commits, docs, tests, soak logs
and session notes, on 2026-10-30. The same kind of session wrote nearly all of the code. Chad Thornton ran the
project, made the decisions, and did everything at the board. Anything the record doesn't settle is marked
**open**.

## Pages

**If you want an ESP32 board to control an FM-1:**
1. [USB MIDI host on the ESP32-S3](usb-host-esp32s3.md): the EspUsbHost fork, the 8-channel limit, the resource
   broker, hubs, claims and class compliance.
2. [What broke in the USB host, and why](usb-host-failures.md): stack overflow, allocation in a critical section,
   in-place host restarts, the `hcd_urb_dequeue` assert, hub tracking races.
3. [The FM-1 on USB and in ROM boot](fm1-usb-and-rom-boot.md): its USB identities, the ROM-boot key over MIDI, the
   JieLi `UBOOT1.00` device, `wl82loader`, and the roughly 8.5 s self-restart.
4. [The FM-1's flash, firmwares and save states](fm1-flash-and-firmwares.md): the flash map, protected areas,
   firmware families, the package catalog, save states.
5. [Porting to other ESP32 boards](porting.md): what's portable, what's per-board, and which ESP32 chips can host
   USB at all.

**If you want to know how the firmware loader stays safe:**

6. [The loader's safety design](loader-safety.md): the rules, the guards, backups, pending records, resume, the
   stale-backup check, and one boot per job.

**If you want to know how robust AI-led coding was here:**

7. [How robustness was earned](robustness.md): Mac tests, golden vectors from the reference tools, interop tests,
   hardware checkpoints, soak runs, review rounds.
8. [Mistakes, and how each was caught](mistakes.md): the honest list.
9. [Open questions](open-questions.md): what nobody has explained yet.

## Conventions

- **Commits** are short hashes in this repository (`git show fc72748`).
- **Paths** are relative to the repository root (`lib/fm1_loader/src/layout.h`).
- **Measured** means a log, a test run or a dump shows it. **Hypothesis** means it fits the evidence but nothing has
  tested it.
- **Kept out on purpose:** JieLi's code, the loader blob and its hash, and the byte values of the ROM-boot key, the
  identity query and the chip key. These pages describe what they do and point to where the code keeps them.
