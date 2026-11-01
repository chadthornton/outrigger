# Beta 1 announcement (draft for the M-VAVE firmware Discord)

Chad posts this himself. Two placeholders to fill before posting:

- `<TEST PLAN LINK>` and `<KNOWN ISSUES LINK>`: the docs are only in the repo, which is private today. Once it's
  public they are `https://github.com/chadthornton/usb-midi-host/blob/master/docs/beta/03-test-plan.md` and
  `.../07-known-issues.md`. If the repo isn't public by posting time, drop both links: the DM below carries the checks.
- The risk line says stock ↔ Felucca is "being tested today". Update it once Chad's second FM-1 has run it.

## The post

> Hey all, I'm looking for 3–5 FM-1 owners to beta test **Outrigger**.
>
> **What it is**
> A palm-sized ESP32 touchscreen that sits on a USB hub next to your FM-1. It does two jobs:
> - Plug in any USB MIDI controller and it plays the FM-1. No laptop.
> - Switch FM-1 firmware right on its screen in about a minute. It backs the FM-1 up first, every time, and your
>   songs come back when you switch back.
>
> Beta 1 comes with stock V15, Felucca 1.8.0 and Skiff 2.1.1. It's free and unofficial, and not affiliated with M-VAVE.
>
> **What you need** (shopping list with links: https://outrigger.music/#need)
> - A Waveshare ESP32-S3-Touch-LCD-1.69, the board we've tested
> - A powered USB-C hub that can charge a laptop through it, with 4 data ports or fewer
> - Your FM-1 and a USB MIDI controller
> - Chrome or Edge on a computer. Install is from the browser and takes about 2 minutes.
> - Ideally, a Transporter recovery kit (XIAO RP2040 + a USB-C pigtail, about $15, no soldering)
>
> **What I'm asking**
> Run 8 quick checks: install, play, unplug and replug, and one firmware switch. That's about an hour. Then use it
> normally for a week and tell me what breaks. Test plan: <TEST PLAN LINK>
>
> **The honest risk:** it rewrites your FM-1's firmware. Felucca ↔ Skiff is tested on hardware, and stock ↔ Felucca is
> being tested today. Worst case is a dark FM-1, which is recoverable with the kit. Known issues: <KNOWN ISSUES LINK>
>
> **Sign up:** reply here or email feedback@outrigger.music with your MIDI controller, your hub (if you have one), and
> whether you have a recovery kit. I'll pick 3–5 folks and DM you.
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
> Full test plan and known issues: <TEST PLAN LINK>, <KNOWN ISSUES LINK>
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
