# Switching FM-1 firmware: what can go wrong

Outrigger can put a different firmware on your FM-1: Felucca, Skiff, SLOOP, M-VAVE's own, and most of the
Firmware Hub catalog. This page explains what happens during a switch, what can go wrong, and how to make it
unlikely. Read it before your first switch.

## What happens when you switch

You pick a firmware on the board's **FM-1** page. Its plan page lists, in plain words, everything the switch will
do. Nothing happens until you hold the yellow button for two seconds. Then, in about a minute:

1. The board restarts and sends your FM-1 into its built-in install mode. **The FM-1's screen goes dark. That's
   normal.**
2. It checks the FM-1 against its latest backup. If anything changed since (a new song, say), it backs the FM-1 up
   first. Your very first backup comes from the board's **Backups** screen, before your first switch.
3. It saves the current firmware's songs and settings on the board.
4. It writes the new firmware, and reads back every piece it writes to check it.
5. It sorts out your songs (next section), then restarts the FM-1 into the new firmware.

MIDI stops while this runs, and comes back when the FM-1 does.

## What happens to your songs and settings

- **Another version of the same firmware** (Felucca 1.6.1 to 1.7.0, say): your songs and settings stay.
  One exception so far: **Skiff 1.x to Skiff 2.x** starts fresh, because Skiff 2.0 changed how it saves.
- **A different firmware** (Felucca to SLOOP, say): the old firmware's songs go on their own shelf on the board.
  The new firmware gets back whatever it had the last time you used it, or starts fresh if this is its first time.
  **Switch back later, and your old songs come back with it.**
- **M-VAVE's stock firmware:** nobody has mapped how it stores its settings yet, so the board leaves that area
  as it is. If presets look scrambled after switching to stock, use **System > Reset Patches** on the FM-1.

## What the board never touches

The board refuses, every time, to write:

- **the FM-1's boot area**, the small part that lets it always get back into install mode;
- **its Bluetooth pairing data**, which is also how the board tells two FM-1s apart;
- **its factory voices.**

## What can go wrong

- **The connection drops mid-write.** A cable gets knocked, the hub loses power, the board loses power. The FM-1 is
  left with half a firmware, and won't start normally. **This is recoverable**: see
  [If your FM-1 won't boot](06-if-your-fm1-wont-boot.md). The key is leaving it switched on.
- **The FM-1's battery runs out mid-write.** Same as above, except you'll need to charge it before retrying.
- **Leftovers from another firmware.** Some firmwares crash when they find another firmware's saved data. The best
  known case is SLOOP (built on Felucca 0.9) to ChoralRoot (Felucca 1.x). The board guards against this by starting
  a different firmware fresh, but it only knows where Felucca-style firmwares keep their data. A firmware that keeps
  data somewhere else could still trip over leftovers. If a newly installed firmware crashes at start, switch back
  to the previous one and tell us which pair it was.

## Before you switch

- **Charge the FM-1 fully.**
- **Use short, good USB-C data cables,** pushed all the way in. Plug the FM-1 into one of the hub's USB-C ports.
- **Keep the hub on its power brick.**
- **Back up first** from the **Backups** screen, if you haven't yet.
- **Don't touch anything until the board shows its result.** The board's screen says
  "Don't unplug or switch off the FM-1" while it writes.
