# Spike: web control panel at `http://fm1hub.local`

Branch `spike/web-ui`. A Move-style web page served by the Waveshare itself, for setup that is painful on the 1.69"
screen. Separate from the browser-based installer on `web-installer`.

## Recommendation

**Build it.** Measured on the Waveshare with the Launchkey and FM-1 on the hub, Wi-Fi plus a deliberately hammered
web page add at most 0.3 ms to an occasional forwarded note (worst case 0.71 ms vs 0.41 ms with Wi-Fi off), and all
2,097 notes arrived. That is a third of one USB frame and far below audibility. So Wi-Fi can stay on by default, and
the loader plan's "Wi-Fi only while downloading" rule can be relaxed (with the one gap below).

What it costs, and what has to be decided first:
1. **The partition change becomes a prerequisite.** Wi-Fi + server + page add 598 KB; the app no longer fits the
   default 1.25 MB slot. The loader plan's decision 5 (recovery app + ~3 MB main app + ~12 MB sector store) already
   leaves room; the web panel is one more reason to make that call before either feature merges.
2. **Server: `esp_http_server`**, as decided below. Nothing in the spike hit a limit of it.
3. **Not measured:** whether Wi-Fi delays the moment the hub *notices* an incoming note (USB interrupt to the client
   task). The forward timer starts after that. Measuring it needs evenly spaced input (a held Launchkey arpeggio,
   ~4 minutes); Chad chose to skip it for now. If it ever matters, pinning the USB tasks to core 1 (Wi-Fi lives on
   core 0) is the lever.

## Decisions (made before building)

| Question | Decision | Why |
|---|---|---|
| HTTP server | ESP-IDF `esp_http_server`, through a thin C++ file | Already inside the Arduino core 3.3.12 libraries, and its precompiled `sdkconfig` has `CONFIG_HTTPD_WS_SUPPORT=y`, so WebSocket costs no new library. One server task whose core and priority are set in `httpd_config_t`: core 0, priority 3, below the USB host tasks (5). |
| Not chosen: ESPAsyncWebServer | Maintained again (ESP32Async fork, v3.12.1 on 2026-09-12, pushed 2026-10-24; me-no-dev's original archived 2025-01) | It adds two libraries (ESPAsyncWebServer + AsyncTCP) and a second network task to tune. Its conveniences (WS broadcast helpers, static gzip serving) are ~60 lines here. Revisit if the page grows many routes. |
| Not chosen: Arduino `WebServer` | | Synchronous, no WebSocket. |
| Name | mDNS `fm1hub.local` (`ESPmDNS`, bundled), plus `_http._tcp` service | Bonjour on the Mac and iPhone resolves it. The device's settings screen shows the IP as a fallback (Android browsers ignore `.local` on some versions). |
| Live state | One WebSocket, `/ws`, text frames of JSON | Status at most 4 per second on change; MIDI monitor batched every 50 ms. The USB task never sends: `on_midi` writes into a fixed lock-free ring (256 events; when full it refuses the new event and counts it, never blocks) and the httpd task drains it. |
| Page | One HTML file (inline CSS + JS, no framework), gzipped at build time into a `const uint8_t[]` in the app | No filesystem partition, so the spike doesn't pre-empt the loader plan's storage decision. Served with `Content-Encoding: gzip`. |
| Wi-Fi setup | No saved network → SoftAP `fm1hub-setup` with a captive page (`DNSServer`, bundled) listing scanned networks; credentials saved in NVS namespace `wifi` | Matches the loader plan's "Getting onto Wi-Fi". The phone joins the hotspot, the captive sheet opens, Chad picks the network. |
| Partition | Spike only: `PartitionScheme=huge_app` (3 MB app) | Today's default scheme caps the app at 1,310,720 B and the app is already 1,005,892 B (76%); Wi-Fi + lwIP + httpd + mDNS don't fit. `huge_app` keeps `nvs` at `0x9000`, so saved settings survive. The real layout stays the loader plan's decision 5. |

### Cost against the current build

| | master (`2461e85`) | spike | change |
|---|---|---|---|
| App size | 1,005,892 B | 1,604,191 B | +598,299 B (+59%) |
| Static RAM | 43,632 B | 83,996 B | +40,364 B |
| Page, gzipped | – | 4,961 B (index) + 1,796 B (setup) | from 13.2 KB + 3.6 KB |
| Free internal DMA-capable heap, lowest seen | 132,556 B (Wi-Fi off) | 88,180 B (Wi-Fi idle), 70,964 B (busy) | |
| Largest free internal block, lowest seen | 51,188 B | 47,092 B (idle), 32,756 B (busy) | |

The heap matters because `midiSend()` allocates each MIDI OUT transfer from DMA-capable internal RAM, which Wi-Fi
competes for. 32 KB of largest free block under load is ample for 64-byte transfers, and no send failed.

### Security model

- **LAN only.** No cloud, no port forwarding, no remote access. The server answers only requests whose `Host` is
  `fm1hub.local`, the device's own IP, or the setup hotspot's IP; this blocks DNS rebinding.
- **Reading is open** on the LAN: status, MIDI monitor, settings values.
- **Writing needs pairing.**
  1. The browser asks to pair; the device shows a 6-digit PIN on its screen for 60 s.
  2. The browser sends the PIN. Three wrong tries end the attempt.
  3. The device returns a random 128-bit token, stored in NVS (up to 4 paired browsers). The page keeps it in
     `localStorage`.
  4. Every write sends it in an `X-Fm1hub-Token` header, never a cookie, so a page on another site that makes the
     browser POST to `fm1hub.local` (CSRF) carries no token. The WebSocket checks `Origin` the same way.
- **Stated plainly:** this is HTTP on the LAN, so anyone sniffing the Wi-Fi can copy a token. Acceptable for a home
  network and a device whose writes are text size and favorites; not acceptable for anything that touches the FM-1's
  flash, which is why the next rule exists.
- **The FM-1's flash is never written from the web.** Install, restore and firmware switch from the web page only
  *request* the operation. The device then shows its own confirm screen and needs the 2-second hold (loader plan,
  safety rule 7). The web page shows progress; it can't skip the hold, and a request expires after 60 s.
- **No MIDI from the web.** The server has no path to `midiSend()`. The MIDI monitor is read-only.

### Conflict with the loader plan

The loader plan says "Wi-Fi is on only while checking or downloading". An always-reachable control panel needs Wi-Fi
on whenever the device runs. The prototype has a Wi-Fi on/off switch, and the latency numbers below decide which
default is safe.

## Measured

### Routing latency (2026-10-26)

**Setup.** Waveshare on the powered hub with the FM-1 (Felucca) and Launchkey Mini MK4 37, 8/8 USB channels, hub
on Wi-Fi (`scooterbot`, -57 dBm). Chad played the Launchkey by hand; every note-on/off was forwarded to the FM-1.
One firmware build throughout; only the mode changed (`tools/web_bench.ts`, 45 s per mode).

**What the timer covers.** From `on_midi()` entry (the controller's IN transfer, dispatched on the USB client task)
to completion of the OUT transfer to the FM-1, via a one-function hook added to the vendored EspUsbHost
(`midiSendCompleteHook`). It includes the router, `midiSend()`'s allocation, and the OUT transfer on the wire.

**Modes.** *wifi_off*: Wi-Fi radio and driver stopped for the run. *idle*: joined, server up, nobody connected.
*busy*: the Mac ran 4 WebSocket monitors plus 4 back-to-back fetch loops (page and status JSON), 1,232 HTTP
responses and 2,292 WS frames in 45 s, far beyond what a person causes. The last mode repeats wifi_off to show
nothing drifted.

| Mode | Notes | min | median | p90 | p99 | max | Failed sends |
|---|---|---|---|---|---|---|---|
| wifi_off | 260 | 0.283 ms | 0.321 ms | 0.336 ms | 0.348 ms | 0.412 ms | 0 |
| idle | 547 | 0.289 ms | 0.322 ms | 0.335 ms | 0.486 ms | 0.577 ms | 0 |
| busy | 625 | 0.241 ms | 0.354 ms | 0.599 ms | 0.649 ms | 0.709 ms | 0 |
| wifi_off (repeat) | 665 | 0.280 ms | 0.321 ms | 0.333 ms | 0.345 ms | 0.425 ms | 0 |

Summaries and every raw sample: `docs/web-ui-bench/` (`runN_<mode>.json`, `runN_<mode>_forward.json`).

**Reading it.** The median doesn't move with Wi-Fi idle; the tail grows by ~0.14 ms idle and ~0.3 ms under abuse.
USB full speed polls once per 1 ms frame, so the hub's added delay stays inside one frame. Nothing was dropped.

**Gap.** Arrival jitter (the `note_gaps` fields) is in the files but meaningless here: hand playing isn't evenly
spaced. See the Recommendation for what that leaves unmeasured.

### Found on the hardware

- **Setup page gave no confirmation, though the join worked.** The ESP32-S3 has one radio: joining the home network
  moves it, and the hotspot, to the router's channel, which drops the phone before the reply arrives. Also the
  settings screen kept showing the hotspot for 30 s after joining. Fixed after the run: the page says what will
  happen *before* sending and treats a dropped connection as expected; the hub waits 1.5 s before joining so the
  reply gets out; the screen shows an "On Wi-Fi: fm1hub.local" banner for 10 s and switches to the address at once;
  a failed join shows on the screen next to the hotspot password.
- **`fm1hub.local` took 5 s from `curl` on the Mac** (IPv4 alone: instant). macOS asks mDNS for an IPv6 address
  first and the hub had none. Browsers race both, so pages were fine. Fixed by enabling link-local IPv6 on the
  station interface, so mDNS can answer.
- The Host check works on the hardware: a request to the hub's IP with `Host: evil.example` gets 403.

## What the prototype does

| Piece | Where |
|---|---|
| Wi-Fi, setup hotspot + captive DNS, mDNS, HTTP routes, WebSocket push, pairing | `firmware/waveshare_169/web.cpp` |
| Latency recorder (PSRAM sample sets, per mode) | `firmware/waveshare_169/bench.cpp` |
| Pairing PIN/tokens, Host/Origin guard, percentile maths, monitor ring, JSON field reader (Mac-tested, `make test`) | `lib/web_core/` |
| Wi-Fi switch, network lines and pairing banner on the device | `lib/host_ui/` |
| Pages | `web/index.html`, `web/setup.html`, embedded by `tools/embed_web.py` |
| Bench driver | `tools/web_bench.ts` (`bun tools/web_bench.ts <ip> <seconds> <out_dir>`) |

API:

| Route | Who | What |
|---|---|---|
| `GET /` | LAN | the panel (the setup page when reached through the hotspot) |
| `GET /api/status`, `WS /ws` | LAN | chain, devices, USB n/8, problem text, settings, network; `/ws` adds MIDI events every 50 ms |
| `POST /api/pair/start`, `POST /api/pair` | LAN | show a PIN on the device; trade it for a token |
| `POST /api/settings` | paired | `text_size`, `typeface`, `wifi`; applied by the loop, saved to NVS, shown on both sides |
| `GET /api/wifi/scan`, `POST /api/wifi` | hotspot, or paired | networks; join (credentials to NVS) |
| `POST /api/wifi/forget` | paired | drop the saved network, reopen the hotspot |
| `GET /api/bench`, `POST /api/bench/start`, `GET /api/bench/raw` | LAN, **spike only** | latency runs; remove before merging |

## Sketch: the firmware library page (not built)

Built on the loader plan's shared, content-addressed sector store and its safety rules. The page only reads the
store and *requests* operations; the device does every check and every flash write.

**Library list.** One row per package: name, author, version, license, size it actually costs (sectors not shared
with others), and badges: *installed on Unit A*, *favorite*, *update*, *beta*, *not redistributable*.
- **Favorites and order:** a star pins a package (never evicted, loader plan "Library housekeeping"); drag to reorder.
  The order is what the device's Firmware screen lists first, so the 1.69" screen shows favorites without scrolling.
  Stored as a small list of package hashes in NVS (`POST /api/library/order`, paired).
- **Space bar:** used / free in the sector store, and what would be evicted next.

**Get firmwares.**
- **From The Firmware Hub catalog:** the page shows `catalog.json` entries the device fetched (`If-Modified-Since`,
  once a day or on demand). "Download" asks the device to fetch, check size + SHA-256 + package rules (loader rule 4),
  and store. Stock V16, Baud Girl and Groove OS are marked *bring your own copy* and have no download button.
- **Upload your own `.fwsc`:** `POST /api/library/upload` (paired), streamed 4 KiB at a time into the store, never
  held whole in RAM. The device runs the same package checks and refuses a failed one. This is how the
  non-redistributable firmwares arrive.

**Units, backups and save states.** Per FM-1 unit: name, installed firmware, baseline and latest backup, save states.
- **Download a backup:** `GET /api/units/<id>/backups/<n>.bin` (paired: backups hold the unit's own Bluetooth data).
- **Install / switch / restore:** a button that sends a request. The device then shows its own confirm screen with the
  2-second hold (loader rule 7); the page shows "Confirm on the hub" and live progress from `/ws` (Enter boot,
  Identify, Backup, Write N of M, Verify, Restart). A request not confirmed within 60 s lapses. There is no route that
  writes FM-1 flash without the hold.

**What never leaves the device.** `wl82loader.bin` is stored as its own object kind; the download routes serve only
packages the catalog lists as redistributable or that the user uploaded, and backups. The store API refuses the
loader kind on every read route, with a Mac-side test like the address guard's.

## Follow-ups

- Verify on hardware: the join confirmation and IPv6 fix above (flashed after the run; see the branch log).
- Remove the open bench routes before anything merges, or put them behind pairing.
- A QR code on the device for the setup hotspot (`LV_USE_QRCODE`, ~10 KB) so the phone joins by camera.
- WebSocket Origin is checked after the 101 handshake (core 3.3.12 builds without the pre-handshake hook); a refused
  socket is closed at once. Fine for a LAN tool; note it if the core ever enables the hook.
- The hotspot password and the pairing PIN live only on the device's screen, which is the trust anchor; anyone who
  can see the screen can pair. That matches a physical device on a desk.
