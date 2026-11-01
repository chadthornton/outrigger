# Beta 1 announcement (draft for the M-VAVE firmware Discord)

Chad posts this himself, once https://outrigger.music/beta/ (the testers' guide, built from these docs) is live.
The post's line on tested switches has to match the Known issues page on the live site.

## The post

An open beta for anyone happy to take known risks; written for firmware authors. One paragraph per line, so it
pastes into Discord as is.

> Hey all, Outrigger beta 1 is up, for anyone happy to take some known risks and tell me what breaks: https://outrigger.music
>
> It's an ESP32-S3 touchscreen running as a USB host on a powered hub next to the FM-1. Two jobs:
> - Plays the FM-1 from a class-compliant USB MIDI controller, no computer.
> - Switches FM-1 firmware on the device. It drops the FM-1 into its install mode, backs up first (the full 1 MB, read twice and compared), writes the app, and reads back everything it writes. Each firmware's songs and settings go on their own shelf on the board and come back when you switch back to it.
>
> Nod to konsumer's fm1buddy, which covers a lot more ground. Outrigger is aiming at the friendlier end: install from the browser, everything on one hub, and switching you don't have to babysit.
>
> Beta 1 comes with stock V15, Felucca 1.8.0 and Skiff 2.1.1. Felucca ↔ Skiff is tested on hardware, stock ↔ Felucca is on the bench now, and the loader's data map only really knows the Felucca lineage. You can add any other firmware from the Firmware Hub's list on outrigger.music, straight from your browser; switches we haven't tested clear the old firmware's data first and keep a copy. Melodee, Tessera and the SLOOP family aren't addable yet, and your own builds come next.
>
> **Known risks, up front:** it writes your FM-1's flash. A cable pulled mid-write leaves half an app. The board retries as long as the FM-1 stays powered, and otherwise the Transporter + Unbricker gets you back. Skiff 2.x writes its pattern bank into stock's resource area (0xEA000–0xFBFFF), so Skiff → stock may come back without them. Full list: https://outrigger.music/beta/#known-issues
>
> **Feedback I'd love:** anything that breaks, and, if you write firmware, where yours keeps its data. That map is what decides which switches are safe, and you know it better than I do.
>
> Hardware, install and a short test plan: https://outrigger.music/beta/
> Source (GPL-3.0): https://github.com/chadthornton/outrigger
> Reply here or email feedback@outrigger.music.
>
> Cheers, Chad

## Tester DM (for anyone who asks for a walkthrough)

> Hey <name>, thanks for trying the Outrigger beta!
>
> 1. **Hardware:** https://outrigger.music/#need. Get the tested Waveshare 1.69 board.
> 2. **Install:** on https://outrigger.music, under "Set up once": plug in your ESP32, click **Check my board**, then
>    **Install**.
> 3. **The checks**, each one should just work:
>    1. install
>    2. first start on the hub (all green on Play)
>    3. play for 10 minutes
>    4. unplug and replug the controller, then the FM-1
>    5. restart Outrigger by unplugging the hub's charger
>    6. change the text size, restart, and check it stuck
>    7. back up the FM-1 (FM-1 screen > Backups, hold)
>    8. switch firmware once: Felucca ↔ Skiff. Ask me before switching Skiff to stock.
> 4. **Then a week of normal use.**
>
> Before your first switch: charge the FM-1, use short USB-C data cables, and don't touch anything until the
> screen shows the result. If a switch stops, **leave the FM-1 switched on** and hold Retry. No recovery kit? Tell
> me before you switch.
>
> Report anything to feedback@outrigger.music with the build from Settings (like `ccbc36d hub+L`), your controller
> and hub, your FM-1 firmware, what happened, and a photo of the screen. "All eight passed" is useful too.
>
> Full guide, with the known issues: https://outrigger.music/beta/
>
> Cheers, Chad

## Sign-up

Open: anyone with the hardware can install from the site. Feedback comes as Discord replies or email to
feedback@outrigger.music. The DM above is for anyone who asks to be walked through it.
