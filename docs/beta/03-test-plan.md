# The test plan

Thanks for testing. Please work through these checks once, in order, then use Outrigger normally for a week. Each
check says what should happen. Anything else is worth an email, even if it fixed itself.

## The checks

1. **Install.** Follow "Set up once" on [outrigger.music](https://outrigger.music/#install): **Check my board**, then
   **Install**.
   *Expected:* Check my board says what's on your ESP32 and changes nothing. Install takes a couple of minutes, sets up
   firmware switching, and says it's done. Outrigger restarts by itself.
2. **First start on the hub.** Wire it up as in [Install and first setup](02-install-and-setup.md).
   *Expected:* within a few seconds the **Play** screen shows the hub, your controller and the FM-1 (with its
   firmware's name), all green.
3. **Play for 10 minutes.** Play the way you normally would: notes, knobs, pads.
   *Expected:* every note sounds, with no stuck or dropped notes, and the Router line's count keeps climbing.
4. **Unplug and replug each device.** One at a time: the controller, then the FM-1. Wait for each to come back.
   *Expected:* its line turns orange (the FM-1's says "not found"), then green again within a few seconds of
   plugging back in, and playing works again.
5. **Restart Outrigger.** Unplug the hub's charger, wait 5 seconds, plug it back in.
   *Expected:* everything comes back on its own, as in check 2.
6. **Change a setting.** In **Settings**, change the text size, then restart Outrigger as in check 5.
   *Expected:* the setting is still there after the restart.
7. **Back up your FM-1.** On the **FM-1** screen, open **Backups** and hold the button.
   *Expected:* it reads the FM-1 twice, writes nothing to it, and says the backup is saved.
8. **Switch firmware once** (a new ESP32 comes with stock V15, Felucca and Skiff). Read
   [Switching FM-1 firmware](04-switching-firmware.md) and the switching part of [Known issues](07-known-issues.md)
   first, then pick another of the three on the **FM-1** screen. If you've saved a lot of Skiff patterns, ask us
   before switching Skiff to stock.
   *Expected:* in about a minute the FM-1 restarts in the new firmware. Switch back, and your songs are where you
   left them.

## Then, a week of normal use

Use Outrigger the way you'd use your FM-1 anyway. We're looking for anything that happens only over time: a dropped
connection, a freeze, a setting that didn't stick.

## How to report

Email [feedback@outrigger.music](mailto:feedback@outrigger.music) with:

- the **build** from Outrigger's Settings screen (for example `ccbc36d hub+L`);
- your **MIDI controller** and **hub** (make and model);
- your **FM-1 firmware** and its version;
- **what happened**, and which check it was, if any;
- a **photo of Outrigger's screen**, if it shows something unexpected.

Good news is useful too: "all eight passed" tells us that setup works.
