# Known issues

A living list for beta 1. If you hit something that isn't here, email
[feedback@outrigger.music](mailto:feedback@outrigger.music).

## What beta 1 has, and doesn't

- **Three FM-1 firmwares come with it.** A new ESP32 gets M-VAVE stock V15, Felucca 1.8.0 and Skiff 2.1.1 at install.
- **SLOOP is left out on purpose.** It's built on an older Felucca, and going from SLOOP to Felucca or Skiff can leave
  data that crashes them. It waits until we know where SLOOP keeps its data.
- **Adding other firmwares isn't in the browser yet.** Other firmwares, and newer versions of these three, need a setup
  outside the browser for now: email us and we'll walk you through it. Playing from a controller doesn't need it.
- **No Wi-Fi.** Beta 1's Install button gives you Outrigger without Wi-Fi and without its own web page. Both are coming.

## Firmware switching

Which switches we've run on a real FM-1, with these exact versions:

| Switch | Tested? |
|---|---|
| Felucca 1.8.0 → Skiff 2.1.1 → Felucca 1.8.0 | **Yes.** Skiff started fresh; Felucca's songs came back. |
| Felucca 1.7.0 → 1.8.0 | **Yes.** Songs kept. |
| Stock V15 → Felucca 1.8.0, and back | **Being tested now.** Until this line says yes, tell us before you try it. |
| Skiff 2.1.1 → stock V15 | **Not yet.** Ask us first (below). |

- **Skiff → stock: ask us first.** Skiff saves its patterns into part of the FM-1's memory where stock keeps its factory
  resources. After many Skiff pattern saves, those resources may be gone, and the board can't put them back. Nobody has
  tested what stock does without them. Email us before switching from Skiff to stock.
- **Felucca's FM6 user presets may not survive a round trip.** Switching away from Felucca and back brings its songs
  and settings back, but may lose the FM6 user presets you saved. Write down any you care about before switching.
- **During a firmware switch the board restarts 2–3 times, and the FM-1 goes dark for about a minute.** That's normal.
  A crash looks different: a safe-mode screen that says where it stopped and "RST to try again". If you see that,
  email us.
- What can go wrong during a switch, and what to do, is on [Switching FM-1 firmware](04-switching-firmware.md).

## Setup

- **If an install stops partway** (the cable came out), install again: it picks up where it stopped.
- **An ESP32 that ran an earlier beta may stop at "storage we can't read".** Nothing was installed. Email us before
  going further: your backups may be in there.
- **The starter firmwares can stop downloading when one of them releases.** The Firmware Hub keeps only each
  firmware's newest file, so a fresh install says a download failed and writes nothing until we update the site.
  Email us if you see it.
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
