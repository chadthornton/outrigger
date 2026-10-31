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

## Don't erase them by accident

**Never tick "Erase device" when you install or update the board.** It wipes every FM-1 backup and every
song shelf on the board, and nothing can bring them back.

## Coming later

Two things aren't in beta 1:

- **copying a backup to your computer;**
- **restoring a backup you choose** on the board.

For now, your songs come back through each firmware's shelf. When you reinstall the board with the new installer
page, it offers to save a copy of the board's backups to your computer first: keep that box ticked.
