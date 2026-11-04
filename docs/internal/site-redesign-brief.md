# outrigger.music: design brief for the second pass

2026-10-31. Chad's review of the page PR #7 shipped, written back as criteria to judge the variations against.
The research behind the first pass is in `docs/internal/firmware-page-research.md`.

## What Chad said

- The layout is "pretty mediocre": **too much copy**, no clear sense of direction, weak **hierarchy**.
- Lead with **two things** an ESP32 board adds to an FM-1:
  1. **A small MIDI host.** Play the FM-1 from a USB MIDI controller. Without it you'd need a mix of TRS and 5-pin DIN
     cables, or a full-featured MIDI hub. It's a simple connector: one cable to the FM-1 and one to the controller,
     carrying power and MIDI to both.
  2. **A firmware loader.** Load any of the 100+ firmwares onto the FM-1 **safely, without a laptop**. It **backs up**
     first, so you can switch between firmwares and never lose your work.
- **Update my board** and **My FM-1 is stuck** are secondary. They belong in a sidebar, FAQ or troubleshooting section.
- Add **"What's an ESP32?"**, with a small picture of the board and Amazon links.
- Much more **progressive disclosure**, and a much better information hierarchy.

## Criteria (each variation is scored on these)

| # | Criterion | How to check it |
|---|---|---|
| C1 | **Less copy.** | Words visible on first load, before opening anything. Today: 55 in the masthead, about 556 on the page. Target: under 150 visible, under 25 in the first screen's text. |
| C2 | **The two things lead.** | Within the first screen at 1280×800, both appear as named, equal-weight headlines. Nothing else claims more weight. |
| C3 | **Clear direction.** | One primary action per screen, visually distinct from everything else. A visitor can say what to do next without scrolling. |
| C4 | **Hierarchy.** | Three levels at most before any expander: claim, the two things, how to get started. Squint test: headings and buttons still read when body text blurs. |
| C5 | **Secondary help is out of the main path.** | Update, stuck FM-1, no port and erase details sit in a sidebar, FAQ or troubleshooting block, collapsed by default. |
| C6 | **"What's an ESP32?" with a picture and where to buy.** | A board picture, one sentence, and buy links, reachable without hunting. |
| C7 | **Progressive disclosure.** | Every detail past the first sentence is behind an expander or a link to a docs page (`docs/beta/`). |
| C8 | **Honest.** | No claim the beta can't keep: the hub the rig needs today, the loader's one-time setup if it still applies, backups on the board only. |
| C9 | **Keeps what works.** | Chrome or Edge on a computer, the Erase warning next to Install (until the new installer engine, which never erases, ships), readable without JavaScript, phone width, dark mode, no CDN. |

## Facts the variations have to respect

- **The rig today:** a powered USB hub. Hub power brick into the hub, the hub's upstream cable into the board, the FM-1
  and the controller into the hub's USB-C ports. One supply runs everything (docs/hardware-notes.md).
- **Controllers with a 5-pin or TRS MIDI out** don't need this: a cable into the FM-1's TRS MIDI IN does it.
- **The loader:** 129 of the Firmware Hub's 130 firmwares parse (loader session 3b). Today a Mac sets up the store
  once, because `wl82loader.bin` can't be published. Beta docs page 5: backups live on the board only. Copying them to
  a computer comes later.
- **One build already covers both:** Hub + Loader is the MIDI Hub plus the loader.
- **The new installer engine** (`esptool-installer`) never erases, so the Erase warning goes away when it ships.
- **Name:** Outrigger. The beta docs already use it.

## Open questions for Chad (on the tracker)

1. Show the hub in the rig picture, or is a hub-free rig coming?
2. How to word "without a laptop" while the loader needs one Mac setup.
3. One Install button (Hub + Loader), or two?
4. Which Amazon listings, and plain links or affiliate?

## The three variations, scored (2026-10-31)

Mock-ups in this folder (`index.html` links them). Install and Buy buttons don't act. Visible words were counted
with every expander closed. The first screen is 1280×800.

| | A · Two doors | B · Sidebar | C · Minimal |
|---|---|---|---|
| C1 visible words | 187 (misses the 150 target) | 170 (misses) | **99** |
| C2 both things in the first screen | yes, as two equal cards | yes, as stacked sections | yes, side by side |
| C3 one primary action | Get started | Install, but Buy on Amazon competes in the sidebar | **Install, in the first screen** |
| C4 hierarchy | claim, cards, install band | claim, features, steps; sidebar parallel | claim, two lines, one button |
| C5 help out of the path | FAQ at the bottom | **sidebar, always beside the story on desktop** | grey band at the bottom |
| C6 ESP32, picture, buy | bottom left | **sidebar card, first screen** | grey band |
| C7 disclosure | links to docs pages | "Instead of what?" and "How is it safe?" expanders | links to docs pages |
| C8 honest | beta note on the loader card | beta note under the feature | **weakest**: "No laptop." headline, and the Mac setup sits in an expander |
| C9 keeps what works | yes | yes | yes |

**Recommendation: C's structure, with B's "Get the board" card.** C is the only one under the word target that shows
the claim, both features and Install in one screen. Its weak spot is the "No laptop" headline while the beta still
needs a Mac setup. Change the headline, or show the beta note on the switching line, until that setup goes away.

## Round 2: Chad's notes on A and B (2026-10-31)

Chad liked A and B. What changed in `a2-two-doors.html` and `b2-sidebar.html` (both generated by `build.py`):

- **Help in two buckets.** "Before you buy" covers what an ESP32, an FM-1 and firmware are, MIDI cables and which
  controllers work. It sits as a sidebar to What you need. "Troubleshooting" covers updating, no port, an FM-1 that
  won't start, a second keyboard and backups. It's its own section.
- **Explorable setup picture.** Tap a part, or its chip, and the drawing zooms to it while a panel opens:
  - Outrigger: three real screens from `make ui-shots`;
  - the hub: its wiring, with power in, the host port and the data ports;
  - the FM-1: what it is;
  - the controller: what works.

  Each panel links down to where to buy it. Without JavaScript, the static picture and every panel show.
- **What you need, spelled out:**
  - **The board:** an ESP32-S3 with native USB. The Waveshare 1.69 works today; the 1.8 AMOLED and 3.5 are expected
    next. "Any ESP32 with a screen" would be wrong: a plain ESP32 can't host USB, and neither can S3 boards whose only
    port goes through a converter chip (docs/boards.md).
  - **The hub's requirements:** its own power input, pass-through charging to the host port, USB-C data ports, USB 2.0.
  - **The FM-1**, and the cables.
- **Feature icons:** the controller drawing over Play, the FM-1 drawing over Switch firmware. Under Switch firmware,
  the Firmware Hub's ten most-installed firmwares show what "firmware" means.
- **No instructions before UI.** The Erase warning and the browser requirement are gone from the page. The browser is
  checked when someone clicks Install, and only then explained. Erase goes away because the installer that ships with
  this page has to be the in-page one (`esptool-installer`), which never erases. ESP Web Tools' dialog is a third-party
  component the page can't trim.
- **New criteria from round 2:** C10, someone who has never heard of an FM-1 can work out what it is and where to get
  one. C11, nothing tells people how to use a piece of UI before they see it.

## Round 3: B, Chad's pick (2026-10-31)

`b3-sidebar.html` (from `build.py`; A2 and B2 stay as they were):

- **Headline:** "One FM-1. Your MIDI controller. A hundred instruments." "Untethered" was wrong, because the setup adds
  cables. The subtitle is gone.
- **Wording:** "USB MIDI controller" throughout. "Keyboard" read as a typing keyboard.
- **Value-first copy.** "Play the FM-1 from your USB MIDI controller", and "Make it a groovebox tonight, a sampler
  tomorrow", where firmware comes second. The four zoom panels are rewritten the same way.
- **Shopping list:** each item says its job ("runs Outrigger", "powers everything").
- **Board:** one tested board, "we expect most ESP32-S3 boards to work, we're still testing, tell us", and the hard
  requirements in "The nitpicky part: picking a board".
- **TF card:** a line in the board notes ("worth having"), plus a card in What's coming. The tested Waveshare 1.69
  has no slot, and card storage isn't built, so the page can't tell people they should have one yet.
- **Firmware:** the letter tiles are gone. In their place, four real FM-1 screenshots (SLOOP, X0X, zp12, winampFM)
  plus "100+ more". The images come from the Firmware Hub and belong to each firmware's author: **get their OK, and
  keep the credit, before this ships.**
- **What's coming:** outrigger.local, recording (then streaming), firmware over Wi-Fi, the catalog on a TF card, a
  bigger MIDI hub (several controllers, Bluetooth MIDI, the ESP32-P4's 16 channels), backups on a computer, and bigger
  screens. Each carries its real status from `docs/internal/firmware-page-research.md`.
- **Setup:** three steps in the sidebar, per Chad's answer on the laptop wording:
  1. install from a computer;
  2. the one-time loader file from a Mac, only for switching;
  3. plug into the hub.
- **Later in round 3:** an "Installation overview" disclosure under the setup steps (browser, loader file, hub,
  switching, updating), and Wi-Fi plus "Troubleshoot from your browser" cards in What's coming.
- **Round 3c (Chad, B3 notes):**
  - "The board" is our word. The page now says "your ESP32" before install and "Outrigger" after.
  - The loader step is gone, and setup is two steps. `wl82loader.bin` is public at jl-uboot-tool's pinned commit
    (raw.githubusercontent.com, CORS open, 24,064 bytes), so the in-page installer can fetch it from there, check
    its SHA-256 and store it during Install, the same thing `make setup-loader` does on a Mac. outrigger.music still
    never hosts it.
  - **Depends on:** the in-page installer writing the blob into the store, and Chad's OK on fetching from that repo
    at install time.
- **Round 3d:**
  - The disclosure is now titled "The nitpicky part: picking the right ESP32".
  - It has a "Likely to work" list from Chad's links: the Waveshare 1.8-inch AMOLED (B0DSVK5576), and the Waveshare
    4.3-inch Touch Board B (B0D925SBYF; with a case, B0DCNSRT31).
  - The firmware FAQ links the Firmware Hub.
  - **Check before calling the 4.3-inch board supported:** it drives an 800×480 RGB-parallel display, and
    docs/boards.md warns those use most of the S3's pins. Confirm GPIO19/20 reach its USB-C port.
- **Round 3e:**
  - The FM-1 links Chad's listing (B0H5VCT7XK).
  - The hub section is a spec list:
    - a charging input;
    - pass-through "host charging";
    - two data ports;
    - four or fewer data ports, since the tested hub's second internal hub chip never came up;
    - no card reader or Ethernet port.
  - Chad's hub is no longer sold. The StarTech 5G2A2CPDB is listed as likely to work: 1 USB-C data, 2 USB-A data,
    PD in with 85 W host charging. It's untested.
- **Round 3g, typography (Chad: "looks a little AI generated"):**
  - **Typeface:** Commissioner, a contemporary humanist sans (OFL). It's self-hosted at 37 KB for weights 400–700,
    Latin only.
  - **Type scale:** 13 / 15 / 17 / 20 / 28 / 36–52. Headline weight drops from 820 to 680, and body text gets 1.6
    line height. Paragraphs wrap with `text-wrap: pretty` and keep to about 36em.
  - **Spacing:** one scale (4 8 12 16 24 32 48 64), with 64 px between sections.
  - **What's coming:** the status pills are now a small stage label with a coloured dot above each title, and the
    cards sit in a 3×3 grid.
  - **Headline:** one line per phrase.
- **Round 3h:**
  - What's coming has three groups: "Over Wi-Fi", "Playing and recording", "For builders".
  - A new card, "Write your own apps": Outrigger is one core with plug-in apps, the core hands out USB, Wi-Fi and
    storage, and every screen renders and tests on a laptop. Its label is "Underway", because a documented way for
    outsiders to add an app isn't there yet.
  - Chad didn't like Commissioner, so B3 has a mock-up-only typeface switcher (`fontswitch.js`, `fonts.css`):
    Commissioner, Source Sans 3, Figtree, Instrument Sans, Hanken Grotesk, Onest, Albert Sans and System. All are
    OFL, self-hosted, Latin, 20–37 KB.
- **Round 3i:**
  - The hub gets the ESP32's treatment: a one-line summary, a likely-to-work hub, and "The nitpicky part: picking
    the right hub".
  - The controller is "MIDI controller with USB connection" with one line: "We suspect any MIDI controller made in
    the last fifteen years works."
  - What you need drops "A"/"An".
  - What's coming has three stages only, with a key: In testing (runs in test builds), Designed (worked out on
    paper), Planned (next up).
- **Round 3k:** Chad picked Commissioner. The switcher and the other six faces are removed (they're in git history).
- **Round 3l:** the headline is "FM-1's new best friend.", with "Plug in your keys. Pop in a new instrument." under it. Chad kept "keys": it parallels the second sentence, and the icon on screen is a music keyboard.
- **Round 3m:**
  - Cut "Prices and stock swing a lot…" and "Plus one USB-C cable…".
  - Stage labels follow the repo:
    - **In testing:** the code runs in test builds. Wi-Fi with its setup hotspot, outrigger.local (served at
      fm1hub.local today), browser troubleshooting (/status, /log, /crash), recording (`spike/audio`), the app core,
      and screens at three sizes.
    - **Designed:** the plan is written, the code isn't. Firmware over Wi-Fi (`docs/package-sources.md`: "Nothing
      here is built yet") and backups on a computer.
    - **Planned:** a bigger MIDI hub, and the catalog on a TF card.
  - Chips appear only once a part is picked. The dots pulse with an outward ring. Each part has an invisible hit area
    22 units wider all round. The zoom keeps at least 440 units in view, so the hub shows its neighbours.
  - Troubleshooting reads as questions.
  - The feature H2s echo the subheading: "Plug in your keys" and "Pop in a new instrument".
- **Round 3n:**
  - "MIDI controller" in the drawing, the chips and the panel, and no more "you already own".
  - Setup step titles use balanced line breaks.
  - FAQ lists drop their last rule.
  - All four What you need items share one structure: a heading, one sentence, a product list (Tested / Likely /
    Buy), then the nitpicky disclosure.
  - A new FM-1 drawing, measured in millimetres from M-VAVE's top-down product photo (161.5 × 96.5 mm):
    - four knobs and OCT−/OCT+ on the left;
    - the square screen;
    - four knobs over a 6×2 button grid;
    - 16 + 11 pill keys, grouped 3-2-3-2-1.
- **Round 3o:** What's coming has two stages, In testing and Planned. "Designed" folds into Planned, and the key is gone.
- **Round 3p:** a new Playing and recording card, "A general-purpose MIDI hub" (Planned). The router recognises only the FM-1 by its USB ID as a destination (`midi_core.cpp`, `device_role`), so other synths need a code change, which the card says. "A bigger MIDI hub" is renamed "More devices at once".
- **Round 3q:** "Bigger screens and more boards" becomes "More ESP32 boards", about broad ESP32-S3/P4 support (screen sizes and shapes, touch or buttons, card slot or not, a profile per board).
- **Round 3r:** the TF card card has a pro tip: buy an ESP32 with a TF slot now, linking the 1.8-inch AMOLED (it has a slot, per docs/boards.md). The tested 1.69 has none.
- **Round 3s:** the likely-to-work hub is now the Aceele 4-port (B0CQJYM5L6): 4 USB-C data ports, 100W PD in, 87 W host pass-through, $15.99, untested. The hub notes add "Plug the charger in first…" (the hub resets its data ports when the charger is unplugged).
