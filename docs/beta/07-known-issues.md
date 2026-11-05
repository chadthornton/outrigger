# Known issues

A living list. If you hit something that isn't here, email
[feedback@outrigger.music](mailto:feedback@outrigger.music).

## What it has, and doesn't

- **Stock V15 comes with it.** A new ESP32 gets M-VAVE stock V15 at install. Add Felucca, Skiff and others from
  the list on the homepage, which comes from [mutant.cloud](https://mutant.cloud) (or the Firmware Hub when
  mutant.cloud is down).
- **SLOOP, and most firmwares built on it or on Felucca 0.9, are left out on purpose.** SLOOP's fourth sample slot
  overwrites the part of the FM-1's memory Outrigger uses to tell FM-1s apart (and the FM-1's Bluetooth address),
  the same problem as Melodee. It waits for the same fix. Six of the family (DX SLOOP, Salt, Hortator, MNM-1,
  Nauclerus, Hawk-Chua) keep their data where Outrigger clears it, by a reading of their source (2026-11-04,
  not yet run on an FM-1), and can be added.
- **Add firmwares from the Firmware Hub's list** on the homepage, up to ten in all: see
  [Adding a firmware](04a-adding-firmware.md). Files of your own (not on the Hub's list) can't be added yet, and
  there's no way to remove a firmware yet.
- **Melodee, Tessera and the firmwares built on Melodee can't be added yet.** They write the part of the FM-1's
  memory Outrigger uses to tell FM-1s apart.
- **Wi-Fi is for remote diagnostics only, and it's off until you turn it on.** In **Setup**, open **Remote
  diagnostics** and tap **Turn on (restarts)**. Outrigger opens a hotspot, `fm1hub-setup`: join it from your phone and
  pick your Wi-Fi. Then open `http://fm1hub.local` on the same Wi-Fi for a read-only page with its status, log and any
  crash. Updates and firmware downloads over Wi-Fi are coming.

## Firmware switching

**Switching is tested on stock and the Felucca family. Every switch between two different firmwares starts on a
clean slate:** it clears the old firmware's data from the FM-1 first. A community firmware's songs and settings wait on its shelf in
Outrigger and come back when you switch back to it. Stock's settings aren't brought back: they're only in the full
backup Outrigger takes before every switch.

- **Skiff → stock: ask us first.** Skiff saves its patterns into part of the FM-1's memory where stock keeps its factory
  resources. After many Skiff pattern saves, those resources may be gone, and the board can't put them back. Nobody has
  tested what stock does without them. Email us before switching from Skiff to stock.
- **During a firmware switch the board restarts 2–3 times, and the FM-1 goes dark for about a minute.** That's normal.
  A crash looks different: a safe-mode screen that says where it stopped and "RST to try again". If you see that,
  email us.
- **After a firmware switch, your MIDI controller may stop sending.** Unplug it and plug it back in.
- What can go wrong during a switch, and what to do, is on [Switching FM-1 firmware](04-switching-firmware.md).

## Setup

- **If an install stops partway** (the cable came out), install again: it picks up where it stopped.
- **An ESP32 that ran an earlier beta may stop at "storage we can't read".** Nothing was installed. Email us before
  going further: your backups may be in there.
- **Adding a firmware needs Outrigger in Computer mode, on a build from this page.** After an install it restarts into
  MIDI: go back to Computer mode before adding.
- **An FM-1 running M-VAVE's October "Open" firmware plays, but can't be backed up or switched yet.** Outrigger
  recognises it (USB ID 3654:7756, "FM1 Community") and your keys play it. A switch stops before anything is
  written, and says so. To switch firmware, first put stock V15 back with M-VAVE's FM1 Workbench or the Transporter.
- **Installing needs Chrome or Edge on a computer.** Phones, Safari and Firefox can't talk to the board's USB port.
- **There's no way back to an earlier build from the website.** See [Install and first setup](02-install-and-setup.md).

## The hub

- **One MIDI controller at a time.** The ESP32-S3 has room for eight USB connections, and the hub, the FM-1 and one
  controller use all eight. A second controller shows its name but doesn't play.
- **On some hubs, some ports never work.** Hubs with more than four data ports usually hide a second hub chip inside,
  and Outrigger can't reach the ports behind it. On the hub we tested, those were its USB-A ports. Use a hub with four
  data ports or fewer.
- **Unplugging the hub's charger restarts everything.** Outrigger runs on the hub's power. Don't unplug it during a
  firmware switch.

## On the board

- **The largest text size pushes some lines off the screen.** Use small or medium for now.
- **Your backups can't be copied to a computer yet.** They live on Outrigger only. See
  [Your backups](05-your-backups.md).

## Getting help

- **Email [feedback@outrigger.music](mailto:feedback@outrigger.music)** with the build from Settings, what happened,
  and a photo of the screen. [The test plan](03-test-plan.md) lists what helps.
- **A dark FM-1 can be brought back.** If a switch stops, keep the FM-1 switched on and see
  [If your FM-1 won't boot](06-if-your-fm1-wont-boot.md). When nothing else works, the Transporter recovery kit
  (about $15, no soldering) writes stock firmware back. We'd rather you had one before your first switch.
