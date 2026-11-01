# Known issues

A living list. If you hit something that isn't here, email
[feedback@outrigger.music](mailto:feedback@outrigger.music).

## Setup

- **Outrigger comes with three firmwares, and adding others isn't in the browser yet.** A new ESP32 gets stock V15,
  Felucca 1.8.0 and Skiff 2.1.1 at install. SLOOP isn't one of them yet: it's built on an older Felucca, and going
  from SLOOP to Felucca or Skiff can leave data that crashes them, so it waits until we know where SLOOP keeps it. Other firmwares, and newer versions of these, need a setup
  outside the browser for now: email us and we'll walk you through it. Playing from a controller doesn't need it.
- **The starter firmwares can stop downloading when one of them releases.** The Firmware Hub keeps only each
  firmware's newest file, so a fresh install says a download failed and writes nothing until we update the site.
  Email us if you see it.
- **If an install stops while it sets up the store** (the cable came out), install again: it picks up where it
  stopped.
- **An ESP32 that ran an earlier beta may stop at "storage we can't read".** Nothing was installed. Email us before
  going further: your backups may be in there.
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

## Firmware switching

What can go wrong during a switch, and what to do, is on [Switching FM-1 firmware](04-switching-firmware.md). If your
FM-1 won't start afterwards, keep it switched on and see [If your FM-1 won't boot](06-if-your-fm1-wont-boot.md).
