# Known issues

A living list. If you hit something that isn't here, email
[feedback@outrigger.music](mailto:feedback@outrigger.music).

## What it has, and doesn't

- **Stock V15 comes with it.** A new ESP32 gets M-VAVE stock V15 at install. Add Felucca, Skiff and others from
  the list on the homepage, which comes from [mutant.cloud](https://mutant.cloud) (or the Firmware Hub when
  mutant.cloud is down).
- **SLOOP, and the firmwares built on it or on Felucca 0.9, are left out on purpose.** SLOOP is built on an older Felucca, and going from SLOOP to Felucca or Skiff can leave
  data that crashes them. It waits until we know where SLOOP keeps its data.
- **Add firmwares from the Firmware Hub's list** on the homepage, up to ten in all: see
  [Adding a firmware](04a-adding-firmware.md). Files of your own (not on the Hub's list) can't be added yet, and
  there's no way to remove a firmware yet.
- **Melodee, Tessera and the firmwares built on Melodee can't be added yet.** They write the part of the FM-1's
  memory Outrigger uses to tell FM-1s apart. Outrigger refuses to switch to them, and to SLOOP's family, even from
  a store set up some other way.
- **No Wi-Fi.** The Install button gives you Outrigger without Wi-Fi and without its own web page. Both are coming.

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
- **A factory-stock FM-1 may not be marked "running" in the firmware list after its first backup,** even though it is.
  Harmless; the fix is coming.
- What can go wrong during a switch, and what to do, is on [Switching FM-1 firmware](04-switching-firmware.md).

## Setup

- **If an install stops partway** (the cable came out), install again: it picks up where it stopped.
- **An ESP32 that ran an earlier beta may stop at "storage we can't read".** Nothing was installed. Email us before
  going further: your backups may be in there.
- **Adding a firmware needs Outrigger in Computer mode, on a build from this page.** After an install it restarts into
  MIDI: go back to Computer mode before adding.
- **An FM-1 running M-VAVE's October "Open" firmware isn't recognised yet.** Its USB ID is 3654:7756 ("FM1 Community").
  Outrigger treats it as a MIDI controller and shows no FM-1. The fix is coming; until then, switch it back to stock
  with M-VAVE's own tools or the Transporter.
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
