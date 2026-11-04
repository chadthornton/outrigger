# Firmware page: research before the redesign

2026-10-30. The research step for redesigning `site/index.html`, the browser installer people use to put this firmware
on a Waveshare board. Chad's brief: much of it "could be better explained", it needs "a better progressive disclosure
approach", and it should be "much more user centric". Assume a visitor doesn't know what this device is.

**Which page:** the public web installer (`make serve`, localhost:8088). The board's on-screen Firmware page is a
different screen, and loader-3a-final owns its layout.

## What the page says today

`site/index.html`, about 300 words:

1. **A one-line pitch** for builders: "computer-free USB MIDI host", "a keyboard and a synth into a powered hub". It
   never says FM-1 or why an FM-1 owner would want this.
2. **You need:** the board, a data cable, Chrome or Edge.
3. **Install:** five steps. Step 1 is for people already running it (Computer mode). The BOOT+RST fallback sits in a
   yellow box in the middle of the page, at the same weight as the main path.
4. **Boards:** one card with three equal green buttons (MIDI Hub, Hub + Loader, Everything). It explains them in one
   dense paragraph.

## What the page gets wrong

- **It's written for the person who built it.** Words like "MIDI host", "app set", "Computer mode" and "download mode"
  appear before anything says what the device does for a musician.
- **Three equal buttons ask a newcomer to choose before they understand.** Nothing marks one as the default.
- **Hub + Loader overpromises.** The card says it "backs up the FM-1 and installs firmware on it". A board installed
  only from this page can't: the loader needs JieLi's `wl82loader.bin` in the store, which may never be published
  (`tools/build_site.py` refuses it), and the store is set up only by `make flash-store` from a Mac. The board shows
  "Store unreadable: make flash-store" (`firmware/waveshare_169/loader_app.cpp`, `open_store()`). New packages also
  come in only from a Mac (`make store-add`, `docs/package-sources.md`).
- **"Everything" promises apps that aren't there.** On `master` it's the Router, the Loader with writes off, and the
  Wi-Fi diagnostics page (`web.cpp`). No Recorder code exists (`APP_RECORDER` is set but unused).
- **The Erase warning undersells the cost.** "Leave Erase device unticked. That keeps your settings." Erase wipes the
  whole 16 MB flash, including:
  - the settings and saved Wi-Fi;
  - the store: every FM-1 backup and save state;
  - the loader file, which only a Mac can put back.

  The checkbox lives inside ESP Web Tools' own dialog, which the page can't change. The warning has to sit next to the
  button, before the click.
- **The recovery steps get the same weight as the main path.** First-time installers on a board running something
  else do need BOOT+RST. Everyone else doesn't.
- **No hardware guidance.** The hardware findings (docs/hardware-notes.md) are the real failure modes, and none of them are on the
  page:
  - only the hub's USB-C ports work;
  - the 8-channel budget: hub + FM-1 + one controller fills it, and a third device shows its name but plays nothing;
  - the hub must be powered, and its power brick runs the whole rig.

## Who arrives

| Visitor | How they got here | What they know | What they fear | The job they came to do |
|---|---|---|---|---|
| **FM-1 owner with a USB-only controller** (Launchkey Mini, most small USB keyboards) | A forum post, the FM-1 subreddit, a link from Chad | The FM-1, USB MIDI from a laptop. Not ESP32 boards, not flashing. | Buying the wrong board. Breaking the FM-1. | "Play my FM-1 from my keyboard without a laptop on the table." |
| **FM-1 firmware hopper** (runs SLOOP, Skiff, Melodee; installs from the Firmware Hub) | The Hub, Reddit, the Felucca community | Custom firmware, Web MIDI installers, backups in editors. Some have used the Unbricker. | Losing songs and presets when switching. A failed install. | "Switch firmwares without a laptop and keep my work." The device's long-term pitch, not ready for them today (see above). |
| **Someone whose FM-1 is stuck** | A search for "FM-1 bricked" | Something went wrong mid-install. | That it's dead for good. | "Get my FM-1 back." **This device can't do that:** reviving a dead unit needs USB_KEY power-on pulses that can't pass through a hub (loader plan, "Not in scope for v1"). The page's job is to say so in one line and send them to the right tools. |
| **Returning user** (already has a board, wants a newer build) | A bookmark | Everything above. | Losing backups on the board. | "Update it, keep my backups." Needs Computer mode, then Install, then *don't* tick Erase. |
| **Curious synth player or maker** | A link, a demo video | Synths. Maybe Arduino. | Wasting money on parts. | "What is this, what does it cost, could I build one?" |

What everyone shares: they don't yet know the device is a small touchscreen box that sits between a USB keyboard and
the FM-1, powered from the same hub. That has to be the first sentence.

## The honest pitch

- **The FM-1 has a 3.5 mm TRS MIDI IN** (`midi-host-prior-art.md`). A keyboard with a DIN or TRS MIDI out needs a
  cable, not this device. The pitch is for **USB-only** controllers, which most small keyboards are.
- **Today:** plays the FM-1 from a USB keyboard with no computer, shows the chain on screen, and blocks every SysEx
  toward the FM-1, so a controller can never send it an install command.
- **Coming:** switching FM-1 firmware from the board with a verified backup first, and save states per firmware. It
  works today only on a board set up from a Mac. The page should say "coming" and point builders at the README, not
  sell it to newcomers.

## What the device is for (Chad, 2026-10-30, after the first mocks)

The first mocks pitched a MIDI hub. Chad: the device has gone well past that. He picked layout B ("pick your
situation") and asked for a pitch built on four things:

1. **A safe, reliable, extensible firmware loader and saver.**
2. **More MIDI devices**, potentially, for a simple DAWless setup.
3. **Wi-Fi and well-considered updaters.**
4. **A "utility player"** that adds extensibility across the FM-1 ecosystem.

Where each stands, from the repo docs (they're newer than the session transcripts):

| Use case | Status | Source |
|---|---|---|
| Play the FM-1 from a USB keyboard, no computer | **Works now** (MIDI Hub) | README milestones 1–3 |
| Switch FM-1 firmware from the board, with a verified backup before every write | **Works after a one-time Mac setup** (Hub + Loader; checkpoint 3 passed 2026-10-30) | loader plan |
| Save states: songs and presets come back when you switch back to a firmware | **Works after Mac setup** (round trip passed at checkpoint 3) | loader plan |
| Tell two FM-1s apart, backups and saves per unit | **Works after Mac setup** | loader plan, "Units and save states" |
| Wi-Fi diagnostics page (`fm1hub.local`) | **Works now**, developers (Everything) | README, "Web diagnostics" |
| Record the FM-1's audio, take it to a laptop over Wi-Fi | **Proven in a spike**: 44.1 kHz stereo, no dropouts, 30 s takes download in about 83 s | `audio-recording-spike.md` |
| Firmware downloads over Wi-Fi from the Firmware Hub catalog, upload your own | **Designed**, not built | `package-sources.md` |
| Backups and saves to a laptop through the web page | **Designed** | loader plan, "Laptop backups" |
| The board updates itself | **Designed** (recovery app slot reserved) | loader plan |
| Bluetooth MIDI: keyboard notes travel by Bluetooth, freeing USB channels to play and record at once | **Proposed**, paper math (7 of 8 channels) | `architecture.md`, "BLE MIDI" |
| More USB MIDI devices on the hub | **Limited by hardware**: 8 host channels; hub + FM-1 + one keyboard fills them. Bluetooth MIDI or the ESP32-P4's 16 channels are the routes out | README, `boards.md` |
| Bigger screens and other boards, a TF card for the whole catalog | **Planned** | `boards.md`, `architecture.md` |

The page has to put pillar 1 first and still be honest that it needs a Mac today. Every capability carries its
status where it's shown.

## How comparable pages explain themselves

| Page | What works | What to borrow |
|---|---|---|
| **The Firmware Hub** (thefirmwarehub.com) | A catalog first; installing is one button per card. The FAQ answers fear head-on: "My FM-1 is stuck: is it bricked? Almost certainly not", then explains the ROM bootloader. "What happens when you press Install" is collapsed. | The calm FAQ tone for fear questions. The "what happens when you press Install" expander. Links for rescue: its Rescue box, the Unbricker, the Transporter. |
| **SLOOP installer** (isod89.github.io/sloop-fm1) | It opens with what the firmware turns the FM-1 into, with four numbers (153 sounds, 37 kits…), before the button. One button. The risk text sits under the button: "Do not unplug the USB cable while writing." | Lead with the outcome, not the mechanism. One button. |
| **Melodee installer** (keremimo.github.io/melodee) | The closest model. Under the title, a three-item need list: Chrome or Edge, a USB data cable, about 3 minutes. Numbered steps. Advanced installs (RC, nightly) collapsed. An "If an installation stopped: Nothing is lost" panel. A "Before you start" list before the write. | The need list with time. Numbered steps. Advanced choices collapsed. Reassurance for an interrupted install. Warnings stated before the action. |
| **WLED installer** (install.wled.me) | Basic and Advanced modes. "No device found?" leads straight to drivers and "Make sure your USB cable supports data transfer." | A Basic default, with the rest behind "Advanced". The "No port?" fix right where it's needed. |
| **ESP Web Tools itself** | The dialog asks about Erase, shows progress, and offers logs. The page can't change it. | The page has to prepare people for the dialog: name the port they'll see and the box to leave unticked. |

## What this means for the page

1. **First screen:** what the device is, in one sentence an FM-1 owner understands, plus a picture of the rig
   (keyboard → hub ← board, FM-1 on the hub).
2. **What you need:** a short checklist covering the board, a powered USB-C hub, the cables, and Chrome or Edge on a
   computer, with how long it takes.
3. **One primary action:** Install, with MIDI Hub as the default. The Erase warning sits beside it, never collapsed.
4. **Behind expanders:**
   - updating a board that already runs it (Computer mode);
   - "No port shows up" (BOOT+RST, data cable, drivers);
   - the other builds (Hub + Loader and what setup it needs; Everything, and what it really adds);
   - "My FM-1 is stuck" (not this device: here's where to go);
   - what Install writes, and what it leaves alone.
5. **The open question for Chad:** whether Hub + Loader stays a visible button on a public page while it needs a Mac
   to set up. The prototype directions differ on this, so his pick decides it.

## What shipped (2026-10-30)

Chad found the rebuilt B over-explained and asked for a strong editorial path. The page in `site/index.html` keeps B's
"what do you want to do?" choice and cuts everything else:

- **Masthead:** one claim, "Switch firmware. Keep every song.", and one paragraph covering the loader, the keyboard and
  what's next.
- **Four tabs:** Play from a keyboard, Switch firmware, Update my board, My FM-1 is stuck. Each has one or two
  sentences, at most three steps and one button.
- **Erase warning:** one line in the warning colour under every Install button, never collapsed.
- **Two expanders:** "No port shows up" and "the FM-1 doesn't play". The Mac setup sits in its own expander under
  Switch firmware.
- **Everything:** a footer link for developers.
- **Without JavaScript,** all four paths show one after another.
