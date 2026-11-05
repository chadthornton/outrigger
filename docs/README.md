# Outrigger docs

Start with the part that fits what you're doing. New here? The [README](../README.md) says what Outrigger is in one
screen, and [outrigger.music](https://outrigger.music) installs it.

## Playing: you have an FM-1 and a controller

The beta guide, also on the site at [outrigger.music/beta](https://outrigger.music/beta/):

1. [What Outrigger is, and what you need](beta/01-what-you-need.md)
2. [Install and first setup](beta/02-install-and-setup.md)
3. [The test plan](beta/03-test-plan.md)
4. [Switching FM-1 firmware: what can go wrong](beta/04-switching-firmware.md), and
   [adding a firmware](beta/04a-adding-firmware.md)
5. [Your backups](beta/05-your-backups.md)
6. [If your FM-1 won't boot](beta/06-if-your-fm1-wont-boot.md)
7. [Known issues](beta/07-known-issues.md)

## Building on another board

- [Boards](boards.md): which ESP32 boards can host the FM-1, what to buy, and how to add a board profile.
- [Hardware notes](hardware-notes.md): the hub, the 8 USB host channels, and what the first bring-up found.
- [The web installer](web-installer.md): how the page flashes a board and what it checks.

## Writing FM-1 firmware

What Outrigger does with your firmware, and where its data lives.

- [Switching FM-1 firmware](beta/04-switching-firmware.md): what a switch keeps, what it never touches, and which
  firmware data survives it.
- [The firmware catalog](firmware-catalog.md): where the Add firmware list comes from (mutant.cloud, the Firmware
  Hub as fallback), how a firmware's family decides a switch, and the daily ecosystem check.
- [Package sources](package-sources.md): how firmware reaches the board (the Firmware Hub catalog, your own files)
  and the trust rule for carrying data between firmwares.
- [The firmware loader](firmware-loader-plan.md): ROM-boot installs, the flash map, units and save states, backups,
  and the safety rules each enforced in code.

## Working on Outrigger

- [Contributing](../CONTRIBUTING.md): build, test, and how changes land and ship.
- [Architecture](architecture.md): one core, pluggable apps, modes and app sets.
- [UI design](ui-redesign.md): the screens, holds, jobs and error wording.
- [Publishing the site](publishing.md): beta and production deploys, rollback, which commit is live.
- [Test bench](test-bench.md): the lab boards and the bench tools that drive them.
- [Recording spike](audio-recording-spike.md): capturing the FM-1's USB audio on the board.
- [Licenses](licenses.md): third-party code and why the project is GPL-3.0.

[internal/](internal/) holds working notes kept for history: design briefs, research and the beta announcement.
They aren't documentation and may be out of date.
