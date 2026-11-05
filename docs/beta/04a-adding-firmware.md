# Adding a firmware

Outrigger comes with M-VAVE stock V15. Add Felucca, Skiff or any other firmware from the community
catalog at [mutant.cloud](https://mutant.cloud), up to ten in all, from the homepage. No Mac tools, and nothing on Outrigger is erased.

## How

1. On Outrigger, open **Settings** (the gear), then **Computer mode**, and hold the button. Outrigger restarts with
   MIDI off.
2. Plug it into your computer with a data cable, and open [outrigger.music](https://outrigger.music/#add) in Chrome or
   Edge.
3. If you haven't updated Outrigger since this feature came out, click **Check my board**, then **Install**, first.
   Outrigger restarts into MIDI after an install, so go back to **Computer mode** (step 1) before going on.
4. Under **Add custom FM-1 firmwares**, click **Connect to my Outrigger** and pick the port Chrome offers. The page lists
   mutant.cloud's FM-1 firmwares, with each one's family and license, and marks the ones Outrigger already has. If
   mutant.cloud doesn't answer within a few seconds, the page uses the Firmware Hub's list instead and says so.
5. Click **Add** next to one. Your browser downloads it from the same site as the list and checks it against the list's
   SHA-256.
   Then it sends it to Outrigger, which checks it again before storing it. Outrigger's screen shows how far it's got.
   About a minute.
6. Add more if you like. When you're done, tap **Back to MIDI** on Outrigger, then plug it back into the hub. The new
   firmware is on the **FM-1** page.

## What it changes

Only Outrigger's store, and only by adding: your backups, your saved songs and the firmwares already there stay as
they are. If the cable comes out partway, nothing is added; try again.

Outrigger refuses an add, and says why, when:

- it already has ten firmwares (its FM-1 page shows ten);
- the add would leave less room than one full backup of your FM-1, which every switch takes first;
- the file doesn't arrive intact, or isn't an FM-1 firmware it can use.

## What isn't on the list yet

The page lists these under **Not on the list yet**, with the reason:

- **Melodee, Tessera and the firmwares built on Melodee.** They write the part of the FM-1's memory Outrigger uses to tell FM-1s apart.
- **SLOOP and most firmwares built on it or on Felucca 0.9** (Clipper and others). SLOOP's fourth sample slot
  overwrites the same part of the FM-1's memory as Melodee. DX SLOOP, Salt, Hortator, MNM-1, Nauclerus and Hawk-Chua
  are on the list: by a reading of their source (not yet run on an FM-1), they keep everything where Outrigger
  clears it on a switch.
- **Firmwares without a license**, and one whose file doesn't pass Outrigger's checks.
- **Files of your own** (an author's test build, V16, a firmware the Hub doesn't list). Only the Hub's list for now;
  your own files come later, behind a setting.

## Switching to an added firmware

The same as switching to the three it came with: see [Switching FM-1 firmware](04-switching-firmware.md). Most
switches to an added firmware haven't been tested on a real FM-1, so Outrigger clears the old firmware's data first
and keeps a copy on its shelf. Switch back and it comes back.

There's no way to remove a firmware from Outrigger yet.
