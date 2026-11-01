# Beta 1 announcement (draft for the M-VAVE firmware Discord)

Chad posts this himself, once https://outrigger.music/beta/ (the testers' guide, built from these docs) is live.
Which switches have been tested is on the guide's Known issues page, not in the post, so the post doesn't change when
stock ↔ Felucca passes.

## The post

> Hey all, I'm looking for 3–5 FM-1 owners to beta test **Outrigger**: https://outrigger.music
>
> It's a little ESP32 touchscreen that sits on a USB hub next to your FM-1. Plug in any USB MIDI controller and it
> plays the FM-1, no laptop. And it switches FM-1 firmware right on its screen, backing up first, every time.
>
> If you've seen konsumer's fm1buddy: it does a ton, and it's great. Outrigger is trying to be a bit friendlier:
> install from your browser in a couple of minutes, and your songs are waiting when you switch back.
>
> Beta 1 comes with three firmwares (stock V15, Felucca, Skiff), the switches we're testing first. Add any other from
> the Firmware Hub's list on outrigger.music, straight from your browser. Switches we haven't tested clear the old
> firmware's data first and keep a copy. Melodee, Tessera and the SLOOP family aren't addable yet. Firmware authors:
> your own builds come next; tell me which ones you want and I'll test those first.
>
> **What you need:** a Waveshare ESP32-S3 touchscreen board, a USB-C hub that can charge a laptop, your FM-1, and a
> USB MIDI controller. The shopping list has links.
>
> **What I'm asking:** 8 quick checks (about an hour), then a week of normal use, and tell me what breaks. It's all in
> the testers' guide: https://outrigger.music/beta/
>
> **The honest risk:** it rewrites your FM-1's firmware. Worst case is a dark FM-1, which is recoverable with a $15
> kit. Known issues are in the guide.
>
> **Sign up:** reply here or email feedback@outrigger.music with your MIDI controller and hub. I'll pick 3–5 folks
> and DM you.
>
> Cheers, Chad

## Tester DM (after Chad picks someone)

> Hey <name>, you're in for the Outrigger beta. Thanks!
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

## Sign-up: how to pick 3–5

Simplest: people reply in the Discord thread or email feedback@outrigger.music, and Chad picks. No form to build.
Pick for:

1. **Has, or will buy, the tested Waveshare 1.69 board and a laptop-charging hub.** Other boards aren't tested yet.
2. **Has a recovery path:** owns the Transporter kit, or has recovered an FM-1 before.
3. **A spread:** a mix of MIDI controllers, and, once stock ↔ Felucca passes Chad's test, at least one person
   with a recovery kit who runs stock.
4. **Chrome or Edge on a computer.**
