# Spike: web control panel at `http://fm1hub.local`

Branch `spike/web-ui`. A Move-style web page served by the Waveshare itself, for setup that is painful on the 1.69"
screen. Separate from the browser-based installer on `web-installer`.

## Decisions (made before building)

| Question | Decision | Why |
|---|---|---|
| HTTP server | ESP-IDF `esp_http_server`, through a thin C++ file | Already inside the Arduino core 3.3.12 libraries, and its precompiled `sdkconfig` has `CONFIG_HTTPD_WS_SUPPORT=y`, so WebSocket costs no new library. One server task whose core and priority are set in `httpd_config_t`: core 0, priority 3, below the USB host tasks (5). |
| Not chosen: ESPAsyncWebServer | Maintained again (ESP32Async fork, v3.12.1 on 2026-09-12, pushed 2026-10-24; me-no-dev's original archived 2025-01) | It adds two libraries (ESPAsyncWebServer + AsyncTCP) and a second network task to tune. Its conveniences (WS broadcast helpers, static gzip serving) are ~60 lines here. Revisit if the page grows many routes. |
| Not chosen: Arduino `WebServer` | | Synchronous, no WebSocket. |
| Name | mDNS `fm1hub.local` (`ESPmDNS`, bundled), plus `_http._tcp` service | Bonjour on the Mac and iPhone resolves it. The device's settings screen shows the IP as a fallback (Android browsers ignore `.local` on some versions). |
| Live state | One WebSocket, `/ws`, text frames of JSON | Status at most 4 per second on change; MIDI monitor batched every 50 ms. The USB task never sends: `on_midi` writes into a fixed ring buffer (drops oldest, never blocks) and the httpd task drains it. |
| Page | One HTML file (inline CSS + JS, no framework), gzipped at build time into a `const uint8_t[]` in the app | No filesystem partition, so the spike doesn't pre-empt the loader plan's storage decision. Served with `Content-Encoding: gzip`. |
| Wi-Fi setup | No saved network → SoftAP `fm1hub-setup` with a captive page (`DNSServer`, bundled) listing scanned networks; credentials saved in NVS namespace `wifi` | Matches the loader plan's "Getting onto Wi-Fi". The phone joins the hotspot, the captive sheet opens, Chad picks the network. |
| Partition | Spike only: `PartitionScheme=huge_app` (3 MB app) | Today's default scheme caps the app at 1,310,720 B and the app is already 1,005,892 B (76%); Wi-Fi + lwIP + httpd + mDNS don't fit. `huge_app` keeps `nvs` at `0x9000`, so saved settings survive. The real layout stays the loader plan's decision 5. |

### Cost against the current build

Filled in from the build: see [Measured](#measured).

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
