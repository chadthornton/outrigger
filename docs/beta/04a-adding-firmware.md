# Adding a firmware

Outrigger comes with three FM-1 firmwares: M-VAVE stock V15, Felucca and Skiff. You can add more from the Firmware
Hub's list, up to ten in all, from the homepage. No Mac tools, and nothing on Outrigger is erased.

## How

1. On Outrigger, open **Settings** (the gear), then **Computer mode**, and hold the button. Outrigger restarts with
   MIDI off.
2. Plug it into your computer with a data cable, and open [outrigger.music](https://outrigger.music/#add) in Chrome or
   Edge.
3. If you haven't updated Outrigger since this feature came out, click **Check my board**, then **Install**, first.
   Outrigger restarts into MIDI after an install, so go back to **Computer mode** (step 1) before going on.
4. Under **Add custom FM-1 firmwares**, click **Connect to my Outrigger** and pick the port Chrome offers. The page lists the
   Firmware Hub's firmwares and marks the ones Outrigger already has.
5. Click **Add** next to one. Your browser downloads it from thefirmwarehub.com and checks it against the Hub's list.
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

- **Melodee and Tessera.** They write the part of the FM-1's memory Outrigger uses to tell FM-1s apart.
- **SLOOP and the firmwares built on it or on Felucca 0.9** (Clipper, Hortator and others). Going from one of them to
  Felucca or Skiff can leave data that crashes them, until we know where they keep it.
- **Firmwares without a license**, and one whose file doesn't pass Outrigger's checks.
- **Files of your own** (an author's test build, V16, a firmware the Hub doesn't list). Only the Hub's list for now;
  your own files come later, behind a setting.

## Switching to an added firmware

The same as switching to the three it came with: see [Switching FM-1 firmware](04-switching-firmware.md). Most
switches to an added firmware haven't been tested on a real FM-1, so Outrigger clears the old firmware's data first
and keeps a copy on its shelf. Switch back and it comes back.

There's no way to remove a firmware from Outrigger yet.
