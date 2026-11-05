# Your backups

## Where they live

On the board itself, in its own storage, about 12 MB. Not on the FM-1, and not online. The board tells FM-1s
apart, so if you use more than one, each gets its own backups.

## What it keeps

- **Full backups:** a copy of all 1 MB of the FM-1, read twice and compared. It keeps your FM-1's **first backup
  ever** for good, plus the **latest one**. A new backup replaces the previous latest one. One exception: a backup
  that holds the only copy of a firmware isn't replaced. That's a firmware installed some other way, that the
  board couldn't download again.
- **Your songs, one shelf per firmware:** every switch saves the current firmware's songs and settings on its
  shelf. Switch back to that firmware, and they come back by themselves. You don't have to do anything.
- **Firmware files:** the firmwares you've installed, so switching back doesn't need a download.

There's room for roughly a dozen firmwares plus their songs. Songs are small.

## Making a backup

**FM-1** page, then **Backups** at the bottom, then hold the yellow button for two seconds. It reads the FM-1
twice and writes nothing to it. Back up after you change songs or settings, and before trying something risky.
(A switch also backs up by itself first whenever the FM-1 changed since its last backup.)

## Reinstalling the board keeps them

Installing or updating the board from the Outrigger page never erases your backups or song shelves. The page
has no erase option. Only a developer command run from a computer, `make flash-store`, wipes them, and nothing
can bring them back after that.

## Coming later

Two things aren't here yet:

- **copying a backup to your computer;**
- **restoring a backup you choose** on the board.

For now, your songs come back through each firmware's shelf, and your backups live only on the board. Saving a
copy to your computer, including before you reinstall the board, comes later, together with export.
