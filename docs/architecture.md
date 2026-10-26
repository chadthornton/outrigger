# Architecture: one core, pluggable apps

Status: **draft, 2026-10-26** (branch `platform-architecture`). The resource broker's rules are built and Mac-tested
(`lib/platform/`, `make test`). Everything else here is the target the apps are built toward; "Not built yet" says
what is missing.

## Why

Five features grew in parallel: the Router (on `master`), the FM-1 firmware Loader (core on `master`), the browser
installer (on `master`), and spikes for recording the FM-1's USB audio (`spike/audio`) and a web control panel
(`spike/web-ui`). Each works alone. Together they fight over the same six things:

| Resource | Size | Who wants it |
|---|---|---|
| USB host channels | 8 on the ESP32-S3 (`soc/usb_dwc_cfg.h`) | every device: hub, FM-1, controllers, USB stick, the FM-1 in ROM boot |
| The FM-1 | one at a time | Router (MIDI out), Loader (SysEx key, then ROM boot), Recorder (audio in) |
| The 2.4 GHz radio | one | Web UI and Loader downloads (Wi-Fi), BLE MIDI (Bluetooth) |
| Flash | 16 MB, 4 MB mapped today | app (1,005,892 B of a 1,310,720 B slot), Loader's sector store, recovery app |
| NVS | one partition at `0x9000` | every app's settings |
| The screen and its task | one LVGL task | every app's pages, confirms and progress |

The answer: **one shared core owns those resources, and apps ask it for them.** An app never opens a USB interface,
the radio or a flash region on its own. App sets are chosen at build time, so an install carries only the apps it
needs, and the core's rules hold whatever set is built.

## The core

### USB host and device registry

- **Today:** `firmware/waveshare_169/app.cpp` runs EspUsbHost (vendored 2.10.1, `lib/EspUsbHost`). Every callback runs
  on the USB client task. `take_snapshot()` gathers devices and interfaces for the screen.
- **Target:** the registry is that snapshot made permanent. Per address it holds a `platform::DeviceDesc` (VID:PID,
  hub flag, a 3-letter tag, each interface's class triple and endpoint count) and the broker's `DevicePlan`. Apps
  read it; only the core writes it.
- **Device identities** (FM-1 stock `4C4A:C755`, Felucca/Skiff `1209:0001`, ROM boot `4C4A:8057`) live in
  `midi_core::device_role()` and `platform::classify()`. They move into the core when `midi_core` is split (below).

### Resource broker (`lib/platform/src/broker.h`, built)

The broker decides, per interface, whether the host claims it, and which app uses it. Its order of work matches
EspUsbHost's: a device enumerates (its EP0 already costs a channel), then the host claims interfaces one by one.

1. **`attach(device)`** plans the whole device when it has enumerated: which interfaces to claim, for which app, at
   what channel cost, and, when the device ends up doing nothing, the two screen lines that explain why.
2. **`should_claim(address, interface)`** answers EspUsbHost's claim decision.
3. **`set_mode(mode)`** forgets every device. A claim can't be taken back without re-enumerating, so **changing mode
   means restarting the USB host** (`usb.end()`/`usb.begin()` or an `esp_restart()`); every device then enumerates
   again under the new mode's rules.
4. **`owner(part)`** says which app may act on the FM-1's MIDI, audio or ROM-boot face right now.
5. **`wanted_mode()`** differs from `mode()` when an FM-1 already in ROM boot appears outside Load mode (the
   loader's resume entry). The core then restarts the host in Load mode.

**Channel cost** is counted from descriptors, not per-device constants: EP0 for every device (hubs included), plus one
per non-control endpoint of each claimed interface.

| Device | Interfaces claimed | Channels |
|---|---|---|
| Hub | hub (interrupt IN) | 2 |
| Controller (Launchkey) | audio control, first MIDI interface (bulk IN + OUT) | 3 |
| FM-1 in Play | audio control, MIDI | 3 |
| FM-1 in Record | audio control, audio streaming (iso IN, alt 1) | 2 |
| FM-1 in ROM boot (Load) | mass storage (bulk IN + OUT) | 3 |
| USB stick (Record) | mass storage | 3 |
| Skiff's CDC serial pair, HID, second MIDI interfaces | never | 0 (EP0 only, if nothing else) |

So the bench rig, hub + FM-1 + Launchkey, is 8 of 8 in Play. The audio spike found that EspUsbHost today also claims
Skiff's audio streaming interface at enumeration, which would make it 9; the broker leaves it unclaimed in Play.

**Holding the FM-1's share.** While no FM-1 is attached, a non-FM-1 device may not claim into the channels an FM-1
would need in this mode (3 in Play and Load, 2 in Record), **but only while that share can still fit**. A device's EP0
costs a channel whatever the broker decides, so once a third device has enumerated, refusing its interfaces gains
nothing and the broker lets it work. In practice this protects Record mode: a USB stick plugged in before the FM-1
stays off ("SAN off: room kept for FM-1") so the FM-1's audio still fits.

**Screen reasons.** A device is explained only when it ends up doing nothing; an FM-1 whose audio is off in Play is
working, so it isn't. Each reason is two lines of at most 33 characters (`midi_core::kProblemLen`), and a test checks
every refusal × kind × mode combination fits. They replace `app.cpp`'s "port X.Y no USB channel" line, so there is one
path for "why isn't this device working", not two.

| Refusal | Line 1 | Line 2 |
|---|---|---|
| No channels | `No USB channel for MPK` | `8 of 8 in use: unplug a device` |
| Held for the FM-1 | `SAN off: room kept for FM-1` | `Plug in the FM-1 first` |
| Loader running | `LAU ignored while loading` | `Routing is off until it's done` |
| Second FM-1 | `One FM-1 at a time` | `Unplug the other FM-1` |
| ROM boot outside Load | `FM-1 is in ROM boot` | `Restarting in Load mode` |
| Stick outside Record | `USB stick unused in Play mode` | `Record mode saves to it` |

**What it needs from EspUsbHost (not built).** The library claims the first MIDI, audio and mass-storage interface of
every device unconditionally (`EspUsbHost.cpp`, the claim decision around line 9430), with no hook. The broker needs a
vendored patch in the style of the existing `ESP_USB_HOST_MAX_SERIAL_PORTS` one: a claim filter,
`bool (*claimFilter)(uint8_t address, uint8_t interfaceNumber)`, checked next to `isMidiInterface`,
`isAudioInterface` and `isMscInterface`, and a callback when a device has enumerated but before it claims anything, so
`attach()` can run first. The same patch lets the Recorder leave the FM-1's MIDI unclaimed.

**Hardware questions the broker can't answer on the Mac:**
- Does a device whose interfaces are all refused still hold its EP0 channel? The broker assumes yes (the README saw an
  over-budget device enumerate with its name but no MIDI). If ESP-IDF frees it, the "held share" rule gets stronger.
- Does EspUsbHost's audio claim really fail silently in the measured 8/8 rig (Launchkey first), or does that Skiff
  build have no audio? The audio spike's firmware shows each claim result.

### Storage

- **Partition layout: the loader plan's decision 5**, chosen and not yet in code. Map all 16 MB: a ~0.75 MB recovery
  app, one ~3 MB main app, and one ~12 MB shared sector store. Changing the table wipes NVS and everything past the
  app once, so it lands once, before any app stores data there.
- **The shared sector store** (`lib/fm1_loader/src/store.h`: `Storage`, `SectorStore`, `Manifest`) becomes a core
  service. Each 4 KiB sector is stored once under its hash; each object is a list of sector hashes. Objects are
  grouped per app: the Loader's packages, units, backups and saves, and the main app's own downloaded updates. The
  storage back end stays abstract, so microSD (the 1.47" Waveshare's TF slot) can replace internal flash.
- **Recordings don't go to flash.** At 176.4 kB/s, flash is too small and slow (audio spike). The Recorder captures
  to PSRAM (8 MB, about 47 s) and saves to a USB stick or SD card.
- **Pinned and evictable.** Backups and saves are pinned. Library packages can be re-downloaded, so the store may
  evict unpinned ones to make room (for example ~3 MB during a self-update).
- **Installs never touch it.** The browser installer writes bootloader, partitions, `boot_app0` and app only;
  `tools/build_site.py` refuses parts that overlap NVS or overflow the app slot. When decision 5 lands,
  `check_layout()` reads the new table and the same guard holds.

### Settings store

NVS, one namespace per app, each with a `schema` key (u8). An app that reads a different schema than it writes
migrates or clears **its own** namespace only. The core never reads another app's keys.

| Namespace | Owner | Keys | Status |
|---|---|---|---|
| `ui` | core (screen shell) | `text_size`, `typeface` | on `master`, no `schema` yet (treat missing as 1) |
| `boot` | core | `computer` (ELF SHA of the build that asked for Computer mode) | on `master` |
| `wifi` | core (radio manager) | saved network | `spike/web-ui` |
| `web` | Web UI | paired browser tokens | `spike/web-ui` |
| `router` | Router | (none yet) | reserved |
| `loader` | Loader | (none yet; unit data lives in the sector store) | reserved |
| `recorder` | Recorder | (none yet) | reserved |
| `ble` | BLE MIDI | (none yet) | reserved |

The **mode is not saved**: the device always starts in Play. Load mode is entered by confirm, or forced by an FM-1
found in ROM boot, never by a stored flag.

### Radio manager

One 2.4 GHz radio. The ESP32-S3 can run Wi-Fi and BLE together (coexistence), but that costs RAM and adds latency, and
nothing here has measured it. The rule until it is measured: **one radio client at a time**, granted as a lease by
the radio manager.

- **Wi-Fi leases:** Web UI, Loader update checks and downloads, the main app's self-update.
- **BLE leases:** BLE MIDI.
- **Routing never depends on the radio.** USB MIDI routing runs whether or not a lease is held.
- **Open conflict, not resolved here:** the loader plan says "Wi-Fi is on only while checking or downloading"; the Web
  UI wants Wi-Fi on whenever the device runs. The web-UI spike's latency measurements (MIDI jitter with Wi-Fi on)
  decide the default. Until then, Wi-Fi is off by default and the Web UI has an on/off switch.
- **Wi-Fi setup** is the web-UI spike's: no saved network opens the `fm1hub-setup` hotspot with a captive page;
  credentials go in NVS `wifi`.

### Screen shell (`lib/host_ui`)

- **One LVGL task** (the Arduino loop). Apps never call LVGL from their own tasks; they hand the shell data.
- **Home is the signal-chain screen** (`status.h` decides each node's health and the one problem worth explaining).
  Broker refusals feed that problem line.
- **Apps add pages**, reached from Settings: Loader's Firmware page, the Recorder's meter, the Web UI's address and
  pairing PIN. Layout comes from the `BoardProfile`, never fixed pixels.
- **The confirm-and-hold widget** (2-second hold, never a single tap) belongs to the shell. Anything that acts on the
  FM-1's flash goes through it, including requests from the Web UI.
- **Computer mode** (`boot_mode.h`) stays a core screen: the USB host is never started, so no app runs.

## Modes

Each mode belongs to one app. Only that app may act on the FM-1. `platform::owner()` reports it and tests check it for
every mode.

| Mode | App | FM-1 MIDI | FM-1 audio | FM-1 ROM boot | Controllers | USB stick |
|---|---|---|---|---|---|---|
| **Play** (default) | Router | Router | off | → restart in Load | Router | off |
| **Record** | Recorder | off | Recorder | → restart in Load | Router (for BLE, see below) | Recorder |
| **Load** | Loader | Loader (SysEx key only) | off | Loader | off: routing stops | off |

- **Load mode is the loader's safety rule 7 made structural.** Controllers' MIDI interfaces are not even claimed, so
  nothing can be routed while the loader runs. The Router still drops every SysEx in every mode, and the ROM-boot key
  lives only in the Loader.
- **Record mode leaves the FM-1's MIDI unclaimed.** Hub 2 + Launchkey 3 + FM-1 audio 2 = 7 of 8. Playing the FM-1
  while recording then needs another MIDI path: the FM-1's own keys, its TRS MIDI IN, or BLE MIDI.
- **Entering a mode** restarts the USB host. Entering Load needs the confirm-and-hold; leaving it happens when the
  install finishes or fails. Play ↔ Record is a Settings choice with no hold.
- **One FM-1 at a time.** A second FM-1, running or in ROM boot, is refused while one is attached.

## Apps

### Router (`lib/midi_core`, on `master`)

- **Claims:** controllers' first MIDI interface; the FM-1's MIDI interface in Play.
- **Interface to the core:** the USB task's MIDI callback, the registry, `broker.owner(Fm1Part::Midi) == Router` before
  any `midiSend()` to the FM-1. Feeds the chain screen.
- **Rules:** only channel voice messages from a controller's cable 0 reach the FM-1; every system message, SysEx
  included, is dropped (`route_to_target()`, tested). In Record mode it keeps controllers up so BLE MIDI can carry
  their notes to the FM-1.
- **Split needed:** `midi_core` holds core pieces today (`status.*`, `boot_mode.*`, the device identities). They move
  to `lib/platform` in their own change, so the Router library is routing only.

### Loader (`lib/fm1_loader`, core on `master`; plan in `docs/firmware-loader-plan.md`)

- **Claims:** in Load mode, the FM-1's MIDI interface (to send the ROM-boot key), then the ROM-boot device
  `4C4A:8057` (mass storage, SCSI over `mscCommand()`).
- **Interface to the core:** the broker (Load mode, ownership), the sector store (packages, units, backups, saves), the
  shell's Firmware page and confirm-and-hold, and in v2 a Wi-Fi lease for catalog checks and downloads.
- **Rules:** the plan's eight safety rules. Two touch the core: the address guard never lets a write leave the FM-1's
  app area, and the **resume entry** (an FM-1 found already in ROM boot) forces Load mode through `wanted_mode()`.
- **Licensing:** `wl82loader.bin` lives on device storage, fetched by the user's own setup; it is never in git, a
  release, a CI artifact or the installer site.

### Recorder (`spike/audio`, `docs/audio-recording-spike.md`; not on `master`)

- **Claims:** in Record mode, the FM-1's audio streaming interface (UAC1, EP `0x84` iso IN, 44.1 kHz stereo 16-bit,
  Skiff's native rate) and a USB stick's mass storage.
- **Interface to the core:** the broker, PSRAM for capture, the shell's meter page, the stick (or SD) for saved files.
- **Rules:** asks for 44.1 kHz explicitly (EspUsbHost's scoring would pick 48 kHz and turn on Skiff's resampler).
  Counts frames against elapsed time, because the library skips bad iso packets silently.
- **Open:** the spike's measurements (dropouts, whether LVGL drawing starves the USB task) are pending.

### Web UI (`spike/web-ui`, `docs/web-ui-spike.md`; not on `master`)

- **Claims:** no USB interface. A Wi-Fi lease, the `httpd` task (core 0, priority 3, below the USB host's 5), NVS
  `web`.
- **Interface to the core:** reads the registry and the chain view; writes only settings; any FM-1 action becomes a
  *request* the device confirms with its own hold, which expires after 60 s.
- **Rules:** LAN only; writes need a pairing token (PIN on the screen); the USB task never blocks on the web (it writes
  a ring the httpd task drains); no path from the web to `midiSend()`.
- **Cost:** Wi-Fi + lwIP + httpd + mDNS + the page add ~598 KB to the app, so the Web UI **needs decision 5's 3 MB app
  slot**. The spike builds with `PartitionScheme=huge_app` for now.

### BLE MIDI (no spike yet)

No branch or spike exists. What is on record: the audio spike's idea of moving the FM-1's MIDI to Bluetooth so
recording and playing fit in 7 of 8 USB channels. As an app it would claim **no USB interface** and a **BLE lease**;
it would be a MIDI source and sink for the Router, under the same SysEx rule. It can't run beside the Web UI until
coexistence is measured.

## App sets and builds

App sets are chosen at build time. The Router is always built; the others are switched by build flags, read through
`lib/platform/src/apps.h`.

| App set | `APPS=` | Flags | Apps |
|---|---|---|---|
| MIDI Hub | `hub` | none | Router |
| Hub + Loader | `hub-loader` | `-DAPP_LOADER=1` | Router, Loader |
| Everything | `everything` | `-DAPP_LOADER=1 -DAPP_RECORDER=1 -DAPP_WEB_UI=1` | Router, Loader, Recorder, Web UI |

- **Boards × app sets:** `make build BOARD=waveshare_169 APPS=hub-loader` builds into `build/<board>/<app set>/`;
  `make site` stages every combination into `site/firmware/<board>/<app set>/manifest.json`. One board today.
- **Honest about `master`:** the Loader, Recorder and Web UI have no firmware code on `master` yet, so the three app
  sets differ only in the app-set name the settings page shows, and `hub-loader` also compiles `lib/fm1_loader` for
  the ESP32-S3. Each set fills in as its app lands. "Everything" will not fit the current 1,310,720 B app slot once the
  Web UI lands; it needs decision 5's partition table first.
- **CI** (`.github/workflows/ci.yml`) runs `make test` and builds every board × app set on each push, then stages the
  installer site with the fixtures present so `build_site.py`'s `wl82loader.bin` scan really runs. Only
  `site/firmware/` is uploaded. Nothing is published.

## How an app lands on `master`

The merge gate in `README.md`: Mac tests, CI green, and a hardware smoke check on the Waveshare (install, then the
Launchkey plays the FM-1 through the hub) before anything lands. An app also lands with:

1. Its row in the mode table and its claims in the broker, with tests.
2. Its NVS namespace with a `schema` key.
3. Its build flag in `apps.h` and the app-set table.

## Not built yet

| Piece | Blocked on |
|---|---|
| Broker wired into `app.cpp` | the EspUsbHost claim-filter patch, then a hardware check that the rig still reaches 8/8 |
| Mode switching UI (Play ↔ Record, Load via confirm) | the broker wiring; the Recorder and Loader UIs |
| Partition layout (decision 5) | one change, before any app stores data in the sector store |
| Sector store on device | the loader's session 2 (`Storage` on LittleFS or microSD) |
| Radio manager | the web-UI spike's latency numbers; a BLE spike |
| `schema` keys, the `midi_core` split | small, after this branch merges |
