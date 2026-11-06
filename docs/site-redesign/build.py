#!/usr/bin/env python3
"""Writes the round-3 mock-up, b3-sidebar.html (direction B, Chad's pick on 2026-10-31).

    python3 docs/site-redesign/build.py

Round 2's generator (A2 and B2 from shared pieces) is in git history; its output files stay as they were.
Wording: "MIDI controller" ("with a USB connection" where it matters), never "keyboard" on its own (Chad: it reads as a typing keyboard). The user's
device is "your ESP32" before install and "Outrigger" after, never "the board".
"""
import html
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent

BUY_BOARD = "https://www.amazon.com/s?k=Waveshare+ESP32-S3-Touch-LCD-1.69"
BUY_35 = "https://www.amazon.com/dp/B0F3WS2W5R"
BUY_AMOLED = "https://www.amazon.com/gp/product/B0DSVK5576"
BUY_43B = "https://www.amazon.com/Waveshare-Capacitive-Development-Resolution-Dual-Core/dp/B0D925SBYF"
BUY_43B_CASE = "https://www.amazon.com/gp/product/B0DCNSRT31"
BUY_FM1_ALI = "https://www.aliexpress.us/w/wholesale-M-Vave-Fm1.html"
BUY_HUB = "https://www.amazon.com/Aceele-Delivery-Multiport-Splitter-MacBook/dp/B0CQJYM5L6"
BUY_FM1 = "https://www.amazon.com/M-VAVE-Synthesizer-Operators-Algorithms-Arpeggiator/dp/B0H5VCT7XK"
HUB_SITE = "https://thefirmwarehub.com/"
FEEDBACK = "mailto:feedback@outrigger.music"
# outrigger.music/chat goes here (site/_redirects, 302 so the invite can change). None leaves /chat out.
CHAT_INVITE = None

# FM-1 screenshots the Firmware Hub hosts. They belong to each firmware's author: credit them, and ask before launch.
SCREENS = [
    ("loom", "LOOM-8", "a groovebox inspired by the OP-XY"),
    ("x0x", "X0X", "909, 808 and two 303s"),
    ("zp12", "zp12", "a 12-bit sampler"),
    ("winampfm", "winampFM", "a synth that looks like Winamp"),
]


def ext(href, text, cls="need-link"):
    return f'<a class="{cls}" href="{href}" target="_blank" rel="noopener">{text}</a>'


RIG = """<figure class="rig-fig" data-rig aria-label="The setup: your MIDI controller and the FM-1 on a USB hub, with Outrigger on the hub">
  <svg viewBox="0 0 640 312" role="img" aria-label="A MIDI controller and the FM-1 plugged into a USB hub, with Outrigger on the hub"><use href="art.svg#rig"/></svg>
  <div class="chips" role="group" aria-label="Tap a part to look closer">
    <button type="button" data-show="controller">MIDI controller</button>
    <button type="button" data-show="outrigger">Outrigger</button>
    <button type="button" data-show="hub">Powered USB hub</button>
    <button type="button" data-show="fm1">FM-1</button>
  </div>
  <section class="panel" data-panel="outrigger">
    <h3>Outrigger</h3>
    <p>A palm-sized touchscreen on the hub. It sends your playing to the FM-1, shows you what's connected, and handles
      every firmware switch, backup first.</p>
    <div class="shots">
      <figure><img class="shot" src="shots/M_play.png" alt="Outrigger's Play screen: hub, controller and FM-1, all green"><figcaption>Everything's connected</figcaption></figure>
      <figure><img class="shot" src="shots/M_fm1_list.png" alt="Outrigger's firmware list: Skiff running, Felucca holding your songs"><figcaption>Your firmwares</figcaption></figure>
      <figure><img class="shot" src="shots/M_plan.png" alt="Outrigger's switch plan for Felucca"><figcaption>What a switch will do</figcaption></figure>
    </div>
    <div class="row"><a class="need-link" href="#board">Get an ESP32 ↓</a><button class="close" type="button" data-reset>Back to the whole setup</button></div>
  </section>
  <section class="panel" data-panel="hub">
    <h3>One hub, one charger</h3>
    <p>Outrigger, the FM-1 and your MIDI controller all plug in here, and one charger powers them all. Pick a hub that can
      charge a laptop through it, and plug Outrigger in where the laptop would go.</p>
    <svg viewBox="0 0 500 190" role="img" aria-label="Hub wiring: charger into the power input, Outrigger on the host port, FM-1 and controller on USB-C data ports"><use href="art.svg#hub-wiring"/></svg>
    <div class="row"><a class="need-link" href="#hub">What to look for ↓</a><button class="close" type="button" data-reset>Back to the whole setup</button></div>
  </section>
  <section class="panel" data-panel="fm1">
    <h3>The FM-1</h3>
    <p>M-VAVE's pocket FM synth, with 27 keys, a speaker and a battery. Its players have written more than a hundred
      alternative firmwares for it, from grooveboxes to samplers.</p>
    <div class="row"><a class="need-link" href="#fm1">Get one ↓</a><button class="close" type="button" data-reset>Back to the whole setup</button></div>
  </section>
  <section class="panel" data-panel="controller">
    <h3>MIDI controller</h3>
    <p>Any MIDI controller with a USB port that works without installing a driver: keys, pads, knobs, faders. Plug it in
      and it plays the FM-1.</p>
    <p class="fine">Has a 5-pin or 3.5 mm MIDI out? A plain MIDI cable into the FM-1 does that part, no Outrigger needed.</p>
    <div class="row"><button class="close" type="button" data-reset>Back to the whole setup</button></div>
  </section>
</figure>"""

SCREEN_STRIP = '<div class="fwshots">' + "".join(
    f'<figure><img src="shots/fw/{f}.png" alt="{n} on the FM-1\'s screen"><figcaption><b>{n}</b> {w}</figcaption></figure>'
    for f, n, w in SCREENS
) + f'<a class="fwmore" href="{HUB_SITE}" target="_blank" rel="noopener"><b>100+</b> more at the Firmware Hub ↗</a></div>'

PLAY = """<h2>Plug in your keys</h2>
<p>Any MIDI controller with a USB connection plays the FM-1: full-size keys, pads and knobs on a pocket synth. No laptop or MIDI adapters, and
  one charger runs it all.</p>"""

SWITCH = f"""<h2>Pop in a new instrument</h2>
<p>Over a hundred community firmwares, and switching should be easy and safe. Outrigger does it in about a minute and
  backs up first.</p>
{SCREEN_STRIP}"""

INSTALL_BTN = """<button class="btn" type="button" data-install="{out}">Install Outrigger</button>
<p class="install-out" id="{out}" role="status" hidden></p>"""

SETUP = f"""<ol class="steps3">
  <li><div><b>Install Outrigger on your ESP32</b><p>Plug it into this computer.</p>
    {INSTALL_BTN.format(out="out-b")}</div></li>
</ol>
<details class="overview"><summary>Installation overview</summary>
  <dl>
    <dt>Install</dt>
    <dd>Chrome or Edge writes Outrigger onto the ESP32 over USB. It never touches your FM-1 or your backups.</dd>
    <dt>First install</dt>
    <dd>The installer also fetches two files from GitHub and checks both: a 24 KB file from the FM-1's chip maker,
      which we can't host, and M-VAVE's stock V15. If a download fails, nothing is written.</dd>
    <dt>On the hub</dt>
    <dd>Outrigger runs on the hub's host port and finds the FM-1 and your controller by itself. Its Play screen turns
      green when notes get through.</dd>
    <dt>Switch firmware</dt>
    <dd>Pick one on the FM-1 screen and hold to confirm. Outrigger backs up the FM-1, writes the new firmware, checks
      every byte and restarts it. About a minute.</dd>
    <dt>Update Outrigger</dt>
    <dd>Settings, then Computer mode. Plug it into a computer and click Install. Your backups stay.</dd>
  </dl>
</details>"""

def pick(tag, href, text):
    """One row of a product list: a short tag, then the link."""
    return f'<li><span class="tag">{tag}</span>{ext(href, text, "")}</li>'


NEED = f"""<div class="need-items">
  <section id="board" class="need-item">
    <svg class="ico" viewBox="0 0 120 140" aria-hidden="true"><use href="art.svg#board"/></svg>
    <div>
      <h3>ESP32-S3 board with a screen</h3>
      <p>This runs Outrigger. We've tested two:</p>
      <ul class="picks">
        {pick("Tested", BUY_BOARD, "Waveshare ESP32-S3 Touch LCD 1.69 ↗")}
        {pick("Tested", BUY_35, "Waveshare ESP32-S3 Touch LCD 3.5 ↗")}
      </ul>
      <p><b>Other ESP32-S3 touchscreen boards should work</b> if their USB-C goes straight to the chip, with no USB-serial
        converter in between. Adding one is usually a short board profile, not new code. Got one?
        <a href="#install" data-request>Request support here</a>.</p>
      <details class="nitpick"><summary>The nitpicky part: will my board work?</summary>
        <ul>
          <li><b>An ESP32-S3.</b> A plain ESP32, C3 or C6 can't connect USB devices. The S2 and P4 can, but Outrigger
            has no build for them yet.</li>
          <li><b>Native USB.</b> The USB-C port goes straight to the chip. Listings say "native USB" or "OTG".</li>
          <li><b>No "one-click download".</b> Those boards route USB through a converter chip that can't host.</li>
          <li><b>16 MB flash and 8 MB PSRAM.</b> Listings say "N16R8". The installer stops on smaller boards.</li>
          <li><b>A screen chip Outrigger knows:</b> ST7789, ST7796 or ILI9341, with CST816 or FT6336 touch. Another
            chip needs driver code as well as a profile.</li>
          <li><b>A TF card slot is worth having</b> for a coming version. The 3.5" has one. The 1.69" doesn't.</li>
          <li><b>Touch helps</b>, but isn't required.</li>
        </ul>
        <p>Tried another board? <a href="{FEEDBACK}">Tell us how it went</a>.</p>
      </details>
    </div>
  </section>
  <section id="hub" class="need-item">
    <svg class="ico" viewBox="0 0 120 50" aria-hidden="true"><use href="art.svg#hub"/></svg>
    <div>
      <h3>Powered USB-C hub</h3>
      <p>We've tested one hub, so check the listing before you buy: it needs its own power and USB-C data ports.
        Outrigger plugs in where a laptop would.</p>
      <ol class="hubsteps">
        <li><b>Power first:</b> the charger into the hub's charging input. The hub needs its own power.</li>
        <li><b>Outrigger on the hub's host cable</b> (or host port): the one a laptop would plug into.</li>
        <li><b>The FM-1 and your controller in the hub's USB-C data ports.</b> On the hub we tested, its USB-A ports
          didn't work.</li>
      </ol>
      <svg class="hubfig" viewBox="0 0 500 190" role="img" aria-label="Hub wiring: charger into the power input, Outrigger on the host cable, FM-1 and controller on USB-C data ports"><use href="art.svg#hub-wiring"/></svg>
      <details class="nitpick"><summary>The nitpicky part: what to look for in the listing</summary>
        <ul>
          <li><b>A charging input</b>, marked "PD in" or "pass-through". A 30 W USB-C charger is plenty.</li>
          <li><b>Host charging.</b> The hub powers the laptop port, and Outrigger runs on it.</li>
          <li><b>Two data ports</b>, for the FM-1 and your MIDI controller.</li>
          <li><b>Four data ports or fewer.</b> Bigger hubs hide a second hub chip that Outrigger can't reach.</li>
          <li><b>No card reader or Ethernet.</b> Each takes one of Outrigger's eight USB connections.</li>
          <li><b>Plug the charger in first</b>, and leave it alone while a firmware switch runs: unplugging it restarts
            everything on the hub.</li>
        </ul>
      </details>
    </div>
  </section>
  <section id="fm1" class="need-item">
    <svg class="ico" viewBox="0 0 162 97" aria-hidden="true"><use href="art.svg#fm1"/></svg>
    <div>
      <h3>FM-1</h3>
      <p>M-VAVE's pocket FM synth, usually $60–100 depending on the seller.</p>
      <ul class="picks">
        {pick("Buy", BUY_FM1, "Amazon ↗")}
        {pick("Buy", BUY_FM1_ALI, "AliExpress ↗")}
        {pick("Specs", "https://www.m-vave.com/product.html?id=fm-1", "M-VAVE's FM-1 page ↗")}
      </ul>
    </div>
  </section>
  <section id="controller" class="need-item">
    <svg class="ico" viewBox="0 0 220 90" aria-hidden="true"><use href="art.svg#keyboard"/></svg>
    <div>
      <h3>MIDI controller with USB connection</h3>
      <p>We suspect any MIDI controller made in the last fifteen years works.</p>
    </div>
  </section>
</div>"""

BEFORE = """<div class="faq">
  <details><summary>What's an ESP32?</summary><p>A small, cheap computer on a chip, found in lots of maker gadgets.
    Outrigger runs on one that comes with a touchscreen and a USB-C port, so there's no soldering.</p></details>
  <details><summary>What's an FM-1?</summary><p>A pocket FM synthesizer from M-VAVE, with 27 keys, a speaker and a
    battery. Its players write their own firmware for it, which is what Outrigger switches between.</p>
    <p><b>Where to buy:</b> M-VAVE's site has <a href="https://www.m-vave.com/product.html?id=fm-1" target="_blank" rel="noopener">the FM-1's specs ↗</a>
    but no shop. Amazon and AliExpress sell it, usually $60–100. <a href="#fm1">Links are under What you need</a>. Be wary of
    sellers asking much more.</p></details>
  <details><summary>What does "firmware" mean here?</summary><p>The software the FM-1 runs. Swap it and the same
    hardware becomes a different instrument: a groovebox, a sampler, a chord machine. Browse them at
    <a href="https://thefirmwarehub.com/" target="_blank" rel="noopener">the Firmware Hub ↗</a>.</p></details>
  <details><summary>My MIDI controller has a 5-pin or 3.5 mm MIDI out</summary><p>Then a MIDI cable into the FM-1's MIDI IN
    plays it, no Outrigger needed. You'd still want Outrigger to switch firmware.</p></details>
  <details><summary>Which MIDI controllers work?</summary><p>USB controllers that work without a driver, which is most of
    them. One at a time for now: see What's coming.</p></details>
</div>"""

# What's coming, in groups: (heading, one-line intro, cards). Each card: title, text, stage label, stage class.
COMING = [
    ("Over Wi-Fi", "Set up, update and check on Outrigger from your phone or laptop, without plugging it in.", [
        ("Wi-Fi", "Join your network from Outrigger's screen, so updates, downloads and its web page need no cable.", "In testing", "test"),
        ("outrigger.local", "Open Outrigger from any browser on your Wi-Fi: status, backups and settings.", "In testing", "test"),
        ("Firmware over Wi-Fi", "Browse the Firmware Hub on Outrigger's screen and download straight to Outrigger.", "Planned", "plan"),
        ("Backups on your computer", "Copy any backup off Outrigger, and restore the one you choose.", "Planned", "plan"),
        ("Update Outrigger over Wi-Fi", "Install a new Outrigger without a USB cable. It's running on our bench now.", "In testing", "test"),
    ]),
    ("Playing and recording", "More of your rig on one hub, and a way to capture what you play.", [
        ("Record the FM-1", "Capture its audio over USB at 44.1 kHz stereo and pull the takes off over Wi-Fi. Streaming comes after.", "In testing", "test"),
        ("A general-purpose MIDI hub", "Route any MIDI controller with a USB connection to any USB synth, not just the FM-1. The router is built. Pointing it at other synths is a small change we haven't made or tested yet.", "Planned", "plan"),
        ("More devices at once", "Several controllers together, Bluetooth MIDI, and twice the USB room on ESP32-P4 boards.", "Planned", "plan"),
        ("The whole catalog on a TF card", 'Boards with a card slot keep every firmware locally, about 60 MB in all.<span class="tip"><b>Pro tip:</b> buying an ESP32 now? Get one with a TF card slot, like the <a href="#board">3.5"</a>.</span>', "Planned", "plan"),
    ]),
    ("More firmwares", "More of the catalog on your FM-1.", [
        ("Riskier firmwares, with a warning", "SLOOP 2.4 and later, and Melodee. They write the part of the FM-1's memory that holds its Bluetooth address, so Outrigger will warn you before it installs one.", "Planned", "plan"),
    ]),
]

COMING_HTML = "".join(
    f'<div class="cgroup"><div class="cgroup-head"><h3>{g}</h3><p>{intro}</p></div><div class="coming">' + "".join(
        f'<article><span class="stage s-{k}">{st}</span><h4>{t}</h4><p>{d}</p></article>' for t, d, st, k in cards
    ) + "</div></div>"
    for g, intro, cards in COMING
)

TROUBLE = """<div class="faq">
  <details><summary>How do I update Outrigger?</summary><div class="answer">
    <p>Outrigger's USB port is busy running your MIDI gear, so first hand it to your computer: in Settings, open
      Computer mode and hold the button. Then plug Outrigger into your computer and click Check my ESP32, then Install. It goes back to
      MIDI by itself afterwards, and your backups stay.</p>
    <div class="shots-sm">
      <figure><img src="shots/M_setup.png" alt="Outrigger's Settings, with Computer mode: for updates over USB"><figcaption>Settings</figcaption></figure>
      <figure><img src="shots/M_computer_confirm.png" alt="Computer mode screen: hold to restart in Computer mode"><figcaption>Hold to restart</figcaption></figure>
    </div></div></details>
  <details id="no-port"><summary>Why can't the installer find my ESP32?</summary><p>Try another cable: charge-only cables never show up. Or
    hold BOOT, tap RST, let go of BOOT, and click Check my ESP32 again.</p></details>
  <details><summary>My FM-1 won't start. Is it broken?</summary><p>It's almost certainly not dead: its install mode lives in ROM.
    <a href="https://thefirmwarehub.com/#rescue" target="_blank" rel="noopener">The Firmware Hub's Rescue</a> or
    <a href="https://github.com/Quixotic7/MvaveFM1Unbricker" target="_blank" rel="noopener">the Unbricker</a> brings it back
    from a computer.</p></details>
  <details><summary>Why doesn't my second MIDI controller play?</summary><p>For now, Outrigger has room
    for one MIDI controller next to the FM-1. Unplug the other one. More than one is coming.</p></details>
  <details id="counts"><summary>Does this site send anything about me?</summary><p>One anonymous count when you press Install,
    unless you untick the box beside that button: which board profile, which edition, the installer's version, whether
    it finished, and the day. No MAC address, chip serial, IP address, browser details or cookies. A
    finished install counts once per board in each browser. If an install fails, you can send us its log with
    "Send this log to Outrigger": you see all of it first, and your board's MAC address is taken out. Page visits are counted by
    <a href="https://www.cloudflare.com/web-analytics/" target="_blank" rel="noopener">Cloudflare Web Analytics</a>, which uses
    no cookies and doesn't fingerprint visitors.</p></details>
  <details><summary>Where do my backups live?</summary><p>On Outrigger itself, never online. Copying them to a computer is
    coming.</p></details>
</div>"""

STYLE = """
  /* Spacing steps: 4 8 12 16 24 32 48 64 96. Type: 13 / 15 / 17 / 20 / 28 / clamp(36-52). */
  .wrap { max-width: 1120px; margin: 0 auto; padding-inline: max(24px, env(safe-area-inset-left)); }
  .topnav { display: flex; align-items: center; gap: 24px; padding-block: 20px; border-bottom: 1px solid var(--line); }
  .topnav .sp { flex: 1; }
  .topnav .link { font-size: .9375rem; color: var(--muted); text-decoration: none; }
  .topnav .link:hover { color: var(--fg); }
  @media (max-width: 560px) { .topnav .wide { display: none; } }
  .eyebrow { font: 500 .6875rem/1.5 var(--mono); letter-spacing: .06em; text-transform: uppercase; color: var(--muted); }

  .layout { display: grid; grid-template-columns: minmax(0, 1fr) 312px; gap: 64px; padding-block: 48px 32px; align-items: start; }
  @media (max-width: 920px) { .layout { grid-template-columns: 1fr; gap: 48px; padding-top: 32px; } }
  .hero h1 { font-size: clamp(1.875rem, 4.2vw, 3.25rem); line-height: 1.08; letter-spacing: -.025em; font-weight: 680; max-width: 14em; }
  .hero .sub { margin-top: 12px; font-size: clamp(1.25rem, 2.2vw, 1.5rem); line-height: 1.35; color: var(--muted); font-weight: 500; letter-spacing: -.01em; }
  .hero .sub span { white-space: nowrap; }
  .hero .rig-fig { margin-top: 32px; max-width: 540px; }

  .features { margin-top: 48px; display: grid; }
  .feature { display: grid; grid-template-columns: 72px minmax(0, 1fr); gap: 24px; padding-block: 32px; border-top: 1px solid var(--line); }
  .feature .ico { width: 72px; color: var(--fg); margin-top: 4px; }
  .feature h2 { font-size: 1.375rem; line-height: 1.25; letter-spacing: -.01em; max-width: 22em; }
  .feature p { margin-top: 10px; color: var(--muted); max-width: 34em; }
  @media (max-width: 560px) { .feature { grid-template-columns: 1fr; gap: 12px; } .feature .ico { width: 56px; } }

  .fwshots { margin-top: 24px; display: grid; grid-template-columns: repeat(5, minmax(0, 1fr)); gap: 16px 12px; max-width: 620px; }
  .fwshots figure { margin: 0; display: grid; gap: 8px; align-content: start; }
  .fwshots img { width: 100%; aspect-ratio: 1; border-radius: 8px; background: #000; box-shadow: 0 0 0 1px var(--line), 0 1px 2px rgb(0 0 0 / .12); }
  .fwshots figcaption { font-size: .8125rem; color: var(--muted); line-height: 1.35; }
  .fwshots figcaption b { color: var(--fg); display: block; font: 500 .75rem/1.35 var(--mono); letter-spacing: .01em; margin-bottom: 1px; }
  .fwmore { display: grid; place-content: center; text-align: center; gap: 2px; aspect-ratio: 1; border-radius: 8px; background: var(--soft);
            text-decoration: none; font-size: .8125rem; line-height: 1.3; color: var(--muted); padding: 8px; }
  .fwmore b { font-size: 1.375rem; font-weight: 650; color: var(--fg); }
  @media (max-width: 560px) { .fwshots { grid-template-columns: repeat(3, minmax(0, 1fr)); } }

  aside.rail { position: sticky; top: 24px; display: grid; gap: 16px; max-height: calc(100vh - 48px); overflow-y: auto; overscroll-behavior: contain; scrollbar-width: none; }
  @media (max-width: 920px) { aside.rail { position: static; max-height: none; overflow: visible; } }
  .card { background: var(--panel); border: 1px solid var(--line); border-radius: var(--radius); padding: 24px; display: grid; gap: 16px; }
  .steps3 { list-style: none; margin: 0; padding: 0; display: grid; gap: 24px; }
  .steps3 b { display: block; font-weight: 600; line-height: 1.35; margin-bottom: 4px; text-wrap: balance; }
  .steps3 p { font-size: .9375rem; line-height: 1.5; color: var(--muted); }
  .steps3 .btn { margin-top: 14px; width: 100%; justify-content: center; }
  .steps3 .install-out { margin-top: 10px; font-size: .875rem; line-height: 1.45; }
  .shopping { list-style: none; margin: 0; padding: 0; display: grid; }
  .shopping a { display: grid; grid-template-columns: 36px 1fr auto; gap: 14px; align-items: center; padding: 12px 0; text-decoration: none; color: inherit; border-bottom: 1px solid var(--line); }
  .shopping li:first-child a { padding-top: 0; }
  .shopping li:last-child a { border-bottom: 0; padding-bottom: 0; }
  .shopping svg { width: 36px; color: var(--fg); }
  .shopping b { display: block; font-size: .9375rem; font-weight: 600; line-height: 1.3; }
  .shopping small { color: var(--muted); font-size: .8125rem; }
  .shopping i { font-style: normal; color: var(--muted); }

  .section { padding-block: 64px; border-top: 1px solid var(--line); }
  .section > h2, .needwrap h2 { font-size: 1.75rem; line-height: 1.2; letter-spacing: -.015em; margin-bottom: 32px; }
  .needwrap { display: grid; grid-template-columns: minmax(0, 1.6fr) minmax(0, 1fr); gap: 64px; }
  .needwrap aside { background: var(--soft); border-radius: var(--radius); padding: 24px 24px 8px; align-self: start; }
  .needwrap aside .faq { margin-top: 8px; border-top: 0; }
  @media (max-width: 920px) { .needwrap { grid-template-columns: 1fr; gap: 40px; } }
  .need-items { display: grid; gap: 40px; }
  .need-item { display: grid; grid-template-columns: 72px minmax(0, 1fr); gap: 24px; align-items: start; }
  .need-item .ico { color: var(--fg); width: 72px; margin-top: 2px; }
  .need-item h3 { font-size: 1.25rem; line-height: 1.3; margin-bottom: 8px; }
  .need-item p, .need-item li { color: var(--muted); max-width: 36em; }
  .need-item p + p { margin-top: 8px; }
  .need-item .picks + p { margin-top: 16px; }
  .need-item b { color: var(--fg); font-weight: 600; }
  @media (max-width: 560px) { .need-item { grid-template-columns: 48px minmax(0, 1fr); gap: 16px; } .need-item .ico { width: 48px; } }
  .picks { list-style: none; margin: 14px 0 0; padding: 0; display: grid; gap: 6px; }
  .picks li { display: grid; grid-template-columns: 64px minmax(0, 1fr); gap: 12px; align-items: baseline; }
  .picks .tag { font-size: .75rem; letter-spacing: .06em; text-transform: uppercase; font-weight: 600; color: var(--muted); }
  .picks a { color: var(--fg); font-weight: 550; }
  .checks { margin: 12px 0 16px; padding-left: 1.1em; display: grid; gap: 8px; }
  .checks li::marker { color: var(--accent); }
  .nitpick p { margin: 4px 0 4px; font-size: .9375rem; }
  .nitpick { margin-top: 20px; border: 1px solid var(--line); border-radius: 10px; padding: 12px 16px; }
  .nitpick summary { cursor: pointer; font-weight: 550; font-size: .9375rem; }
  .nitpick ul { margin: 12px 0 4px; padding-left: 1.1em; display: grid; gap: 8px; font-size: .9375rem; line-height: 1.5; }

  .coming { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 16px; }
  @media (max-width: 860px) { .coming { grid-template-columns: repeat(2, minmax(0, 1fr)); } }
  @media (max-width: 560px) { .coming { grid-template-columns: 1fr; } }
  .coming article { background: var(--panel); border: 1px solid var(--line); border-radius: var(--radius); padding: 24px; display: grid; gap: 8px; align-content: start; }
  .coming .stage { display: flex; align-items: center; gap: 8px; font-size: .75rem; letter-spacing: .06em; text-transform: uppercase; font-weight: 600; color: var(--muted); margin-bottom: 4px; }
  .coming .stage::before { content: ""; width: 7px; height: 7px; border-radius: 50%; background: var(--dot, var(--line)); }
  .coming .s-test { --dot: var(--accent); } .coming .s-plan { --dot: var(--muted); }
  .news { list-style: none; margin: 0; padding: 0; display: grid; gap: 28px; max-width: 760px; }
  .news time { display: block; font-size: .75rem; letter-spacing: .06em; text-transform: uppercase; font-weight: 600; color: var(--muted); margin-bottom: 8px; }
  .news ul { margin: 0; padding-left: 1.1em; display: grid; gap: 8px; }
  .news li li { line-height: 1.55; }
  .coming h4 { margin: 0; font-size: 1.0625rem; line-height: 1.35; font-weight: 650; text-wrap: balance; }
  .cgroup + .cgroup { margin-top: 48px; }
  .faq .answer { margin: -4px 0 18px; display: grid; gap: 14px; }
  .faq .answer p { color: var(--muted); max-width: 38em; }
  .shots-sm { display: flex; gap: 14px; }
  .shots-sm figure { margin: 0; display: grid; gap: 6px; justify-items: center; }
  .shots-sm img { width: 120px; border-radius: 18px; border: 3px solid var(--fg); background: #000; }
  .shots-sm figcaption { font-size: .8125rem; color: var(--muted); }
  .add-steps { margin: 0; padding-left: 20px; display: grid; gap: 6px; font-size: .9375rem; line-height: 1.45; }
  .cgroup-head { display: flex; align-items: baseline; gap: 12px; flex-wrap: wrap; margin-bottom: 16px; }
  .cgroup-head h3 { font-size: 1.125rem; line-height: 1.3; }
  .cgroup-head p { color: var(--muted); font-size: .9375rem; }
  .coming p { color: var(--muted); font-size: .9375rem; line-height: 1.55; }
  .coming .tip { display: block; margin-top: 10px; padding: 10px 12px; border-radius: 8px; background: color-mix(in srgb, var(--accent) 10%, transparent); color: var(--fg); font-size: .875rem; line-height: 1.45; }
  .coming .tip b { font-weight: 650; }
  /* The real install area (site/installer/area.js, from PR #14): buttons, then the board readout, progress and result. */
  .install { display: grid; gap: 12px; margin-top: 12px; }
  .install button { background: var(--accent); color: var(--accent-fg); border: 0; border-radius: 10px; padding: 13px 22px;
                    font: inherit; font-weight: 600; line-height: 1.2; cursor: pointer; width: 100%; }
  .install button.alt { background: transparent; color: var(--fg); box-shadow: inset 0 0 0 2px var(--line); }
  .install p { margin: 0; font-size: .875rem; line-height: 1.45; }
  .install-note { color: var(--muted); }
  .install-read { font-weight: 600; }
  .install-error { font-weight: 600; color: var(--warn); }
  /* Add a firmware (site/installer/add_area.js): the Hub's list, one row each. */
  .add-list { list-style: none; padding: 0; margin: 0; display: grid; }
  .add-list li { display: flex; align-items: center; justify-content: space-between; gap: 12px; padding: 6px 0; border-bottom: 1px solid var(--line); font-size: .9375rem; }
  .add-list button { padding: 6px 16px; width: auto; flex: none; }
  .add-list small { color: var(--muted); }
  .install-done { display: grid; gap: 6px; border-left: 3px solid var(--accent); padding-left: 12px; }
  .install-version { font-size: .75rem; color: var(--muted); }
  .install button:disabled { opacity: .45; cursor: default; }
  .install-boards { border: 0; margin: 0; padding: 0; display: flex; flex-wrap: wrap; gap: 6px 18px; font-size: .9375rem; }
  .install-boards legend { padding: 0; margin-bottom: 6px; font-weight: 600; font-size: .875rem; }
  .install-boards label { display: flex; gap: 6px; align-items: center; cursor: pointer; }
  .install-boards input { accent-color: var(--accent); margin: 0; }
  .install-links { display: grid; gap: 6px; font-size: .875rem; }
  .install-count { display: flex; gap: 8px; align-items: baseline; font-size: .8125rem; color: var(--muted); cursor: pointer; }
  .install-count input { accent-color: var(--accent); margin: 0; }
  .install-links a { text-decoration: none; }
  .install-links a:hover, .install-links a:focus-visible { text-decoration: underline; }
  .report-choice { display: grid; gap: 14px; }
  .install-log { border: 0; padding: 0; font-size: .8125rem; color: var(--muted); }
  .install-log pre { margin: 6px 0 0; max-height: 14em; overflow: auto; white-space: pre-wrap; font: .75rem ui-monospace, "SF Mono", Menlo, monospace; }
  .install .check { display: flex; gap: 8px; align-items: baseline; }
  .install progress { width: 100%; accent-color: var(--accent); }
  /* "For builders": its own dark band, with the page's colours redefined inside it. */
  .nerd { --bg: #0e1622; --fg: #e4eaf3; --muted: #9aa7ba; --line: #263246; --panel: #152033; --soft: #1a2639;
    --accent: #6fb8ff; --accent-fg: #07121f; background: var(--bg); color: var(--fg); margin-top: 64px; padding-block: 64px; }
  .nerd h2 { margin-bottom: 24px; }
  .nerd .eyebrow { margin-bottom: 8px; }
  .nerd a { color: var(--accent); }
  .nerd { --link: var(--accent); }
  .nerd:not(footer) { padding-bottom: 24px; }
  footer.nerd { margin-top: 0; padding-block: 0 48px; }
  footer.nerd .fine { margin-top: 0; padding-top: 24px; }
  .nerd .fine { margin-top: 24px; max-width: 760px; }
  /* Installs with / Licenses / Built on: label left, content right; stacked on a phone. */
  .credits { margin: 48px 0 0; display: grid; border-top: 1px solid var(--line); }
  .credits > div { display: grid; grid-template-columns: 160px minmax(0, 1fr); gap: 8px 32px; padding-block: 20px; border-bottom: 1px solid var(--line); }
  .credits dt { font: 500 .6875rem/1.9 var(--mono); letter-spacing: .06em; text-transform: uppercase; color: var(--muted); }
  .credits dd { margin: 0; font-size: .9375rem; line-height: 1.55; max-width: 40em; }
  .credits dd p + p { margin-top: 4px; color: var(--muted); }
  .credits ul { list-style: none; margin: 0; padding: 0; display: grid; gap: 6px; }
  .credits li span { color: var(--muted); }
  .credits li span::before { content: "· "; }
  @media (max-width: 560px) { .credits > div { grid-template-columns: 1fr; } }
  .addfw { margin-top: 24px; border: 1px solid var(--line); border-radius: var(--radius); background: var(--panel); }
  .addfw summary { cursor: pointer; padding: 14px 18px; display: flex; flex-wrap: wrap; gap: 4px 10px; align-items: baseline; font-size: .9375rem; }
  .addfw summary span { color: var(--muted); }
  .addfw summary b { color: var(--accent); font-weight: 650; }
  .addfw-body { padding: 0 18px 18px; }
  .hubsteps { margin: 12px 0 0; padding-left: 20px; display: grid; gap: 6px; font-size: .9375rem; color: var(--muted); }
  .hubsteps b { color: var(--fg); font-weight: 600; }
  .hubfig { margin-top: 12px; max-width: 420px; color: var(--fg); }
  .addfw-link { display: flex; flex-wrap: wrap; gap: 4px 10px; align-items: baseline; padding: 14px 18px; font-size: .9375rem; text-decoration: none; }
  .addfw-link span { color: var(--muted); }
  .addfw-link b { color: var(--accent); font-weight: 650; }
  .addfw-body p { margin-top: 0; max-width: none; }
  footer .install { margin: 8px 0 12px; max-width: 320px; }
  footer .install button { padding: 8px 14px; font-size: .875rem; width: auto; }
  /* "Report this board" (site/installer/report_form.js): a dialog on the body, so it carries its own button styles. */
  .report { width: min(560px, calc(100vw - 32px)); max-height: calc(100dvh - 32px); overflow: auto; border: 1px solid var(--line);
    border-radius: 14px; background: var(--panel); color: var(--fg); padding: 22px; }
  .report::backdrop { background: rgb(0 0 0 / .45); }
  .report form { display: grid; gap: 14px; }
  .report h2 { margin: 0; font-size: 1.25rem; }
  .report p { margin: 0; font-size: .9375rem; line-height: 1.5; }
  .report-read { background: var(--soft); border-radius: 10px; padding: 12px 14px; display: grid; gap: 8px; }
  .report-log { background: var(--soft); border-radius: 10px; padding: 12px 14px; margin: 0; max-height: 14em; overflow: auto; white-space: pre-wrap; font: .75rem ui-monospace, "SF Mono", Menlo, monospace; }
  .report-read summary { cursor: pointer; font-size: .875rem; color: var(--muted); }
  .report-read dl { display: grid; grid-template-columns: auto 1fr; gap: 2px 14px; margin: 8px 0 0; font-size: .875rem; }
  .report-read dt { color: var(--muted); }
  .report-read dd { margin: 0; overflow-wrap: anywhere; }
  .report-small, .report-field small { font-size: .8125rem; color: var(--muted); line-height: 1.45; }
  .report-field { display: grid; gap: 4px; font-size: .875rem; font-weight: 600; }
  .report-field small { font-weight: 400; }
  .report input, .report textarea { font: inherit; font-weight: 400; font-size: 1rem; color: var(--fg); background: var(--bg);
    border: 1px solid var(--line); border-radius: 8px; padding: 9px 11px; width: 100%; box-sizing: border-box; }
  .report textarea { resize: vertical; }
  .report-trap { position: absolute; left: -10000px; width: 1px; height: 1px; overflow: hidden; }
  .report-status:empty { display: none; }
  .report-status { color: var(--warn); font-weight: 600; }
  .report-status.report-sending { color: var(--muted); font-weight: 400; }
  .report .row { display: flex; flex-wrap: wrap; gap: 10px; }
  .report button { background: var(--accent); color: var(--accent-fg); border: 0; border-radius: 10px; padding: 11px 20px;
    font: inherit; font-weight: 600; cursor: pointer; }
  .report button.alt { background: transparent; color: var(--fg); box-shadow: inset 0 0 0 2px var(--line); }
  .report button:disabled { opacity: .45; cursor: default; }
  footer p + p { margin-top: 6px; }
  .overview { border-top: 1px solid var(--line); padding-top: 16px; }
  .overview summary { cursor: pointer; font-weight: 550; font-size: .9375rem; }
  .overview dl { margin: 12px 0 0; display: grid; font-size: .875rem; line-height: 1.5; }
  .overview dt { font-weight: 600; margin-top: 12px; }
  .overview dt:first-child { margin-top: 0; }
  .overview dd { margin: 2px 0 0; color: var(--muted); }
  footer { border-top: 1px solid var(--line); padding-block: 24px 48px; }
"""

BODY = f"""<nav class="wrap topnav">
  <a class="wordmark" href="#"><i></i>Outrigger</a><span class="beta">Beta</span>
  <span class="sp"></span>
  <a class="link wide" href="#need">What you need</a><a class="link wide" href="#new">What's new</a><a class="link wide" href="#coming">What's coming</a><a class="link" href="#trouble">Help</a>
</nav>
<div class="wrap">
  <div class="layout">
    <main>
      <section class="hero">
        <h1>FM&#8209;1's little companion</h1>
        <p class="sub">Your keys, any firmware, <span>one cheap ESP32</span></p>
        {RIG}
      </section>
      <div class="features">
        <section class="feature">
          <svg class="ico" viewBox="0 0 220 90" aria-hidden="true"><use href="art.svg#keyboard"/></svg>
          <div>{PLAY}</div>
        </section>
        <section class="feature">
          <svg class="ico" viewBox="0 0 162 97" aria-hidden="true"><use href="art.svg#fm1"/></svg>
          <div>{SWITCH}</div>
        </section>
      </div>
    </main>

    <aside class="rail" id="install">
      <div class="card"><p class="eyebrow">Set up once</p>{SETUP}</div>
      <div class="card">
        <p class="eyebrow">Shopping list</p>
        <ul class="shopping">
          <li><a href="#board"><svg viewBox="0 0 120 140" aria-hidden="true"><use href="art.svg#board"/></svg><span><b>ESP32-S3 board</b><small>runs Outrigger</small></span><i>↓</i></a></li>
          <li><a href="#hub"><svg viewBox="0 0 120 50" aria-hidden="true"><use href="art.svg#hub"/></svg><span><b>Powered USB-C hub</b><small>powers everything</small></span><i>↓</i></a></li>
          <li><a href="#fm1"><svg viewBox="0 0 162 97" aria-hidden="true"><use href="art.svg#fm1"/></svg><span><b>FM-1</b><small>the synth</small></span><i>↓</i></a></li>
          <li><a href="#controller"><svg viewBox="0 0 220 90" aria-hidden="true"><use href="art.svg#keyboard"/></svg><span><b>MIDI controller with USB</b><small>keys, pads or knobs</small></span><i>↓</i></a></li>
        </ul>
      </div>
    </aside>
  </div>

  <section class="section" id="need">
    <div class="needwrap">
      <div><h2>What you need</h2>{NEED}</div>
      <aside><p class="eyebrow">FAQ</p>{BEFORE}</aside>
    </div>
  </section>
  <section class="section" id="new"><h2>What's new</h2>
    <ol class="news">
      <li><time datetime="2026-11-05">Nov 5</time><ul>
        <li><b>Six more firmwares can be added:</b> DX SLOOP, Salt, Hortator, MNM-1, Nauclerus and Hawk-Chua, from the
          SLOOP and Felucca 0.9 family. We cleared them by reading their source. None has run on an FM-1 yet.</li>
        <li><b>Newer FM-1s with the UC25HQ80 flash chip can switch firmware.</b> Outrigger warns you first: we haven't
          tested that chip, and a switch may fail or not keep your songs.</li>
        <li><b>M-VAVE's Open firmware is recognised.</b> An FM-1 running it plays from your keys. Backing it up and
          switching away from it come next.</li>
        <li><b>A factory FM-1 on stock V15 shows as running</b> in the firmware list.</li>
        <li><b>Remote diagnostics,</b> off unless you turn it on in Setup. Then Outrigger shows its status, log and any
          crash on a read-only page over Wi-Fi, so we can help without guessing.</li>
        <li><b>Send this log to Outrigger.</b> When an install fails, one button sends us its log. You see exactly what
          goes before you send it, and MAC addresses are removed.</li>
        <li><b>Stock V15 comes from M-VAVE's own repo,</b> at a fixed version, so it can't vanish when a newer one
          comes out.</li>
      </ul></li>
      <li><time datetime="2026-11-04">Nov 4</time><ul>
        <li><b>Add firmware from the community catalog.</b> The list comes from <a href="https://mutant.cloud/" target="_blank" rel="noopener">mutant.cloud ↗</a>, about 190 FM-1 firmwares,
          with the Firmware Hub as a fallback when it's slow. <a href="library/">Add one.</a></li>
      </ul></li>
      <li><time>Since the first beta</time><ul>
        <li><b>Your FM-1 stays recoverable.</b> Outrigger backs it up before every switch and never writes the part of
          the FM-1 that lets it start up and recover. If an install of Outrigger stops partway, install again: it picks
          up where it stopped.</li>
      </ul></li>
    </ol>
  </section>
  <section class="section" id="coming"><h2>What's coming</h2>
    {COMING_HTML}</section>
  <section class="section" id="trouble" style="max-width:760px"><h2>Troubleshooting</h2>{TROUBLE}</section>
</div>
<footer><div class="wrap fine">Firmware screenshots by their authors, via the Firmware Hub. Not affiliated with M-VAVE or Waveshare.</div></footer>
<script src="rig.js"></script>
"""

HEAD = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Outrigger: B3</title>
<link rel="stylesheet" href="shared.css">
<style>{style}</style>
</head>
<body>
"""

MOCK = HEAD.format(style=STYLE) + BODY + "</body>\n</html>\n"
(HERE / "b3-sidebar.html").write_text(MOCK)
print("wrote b3-sidebar.html")


# The real page: the same page with the real install area (site/installer/area.js) in place of the mock button, and
# setup as it is today. The Mac step stays until the installer fetches the loader file itself (tracker: site-loader-fetch).
SITE = HERE.parent.parent / "site"
sys.path.insert(0, str(HERE.parent.parent / "tools"))
import boards  # noqa: E402


def install_area(label, app_set):
    """One install button per built board (tested ones, tools/boards.py ids), plus "another board", which only checks
    the chip; the area asks which board first (site/installer/area.js). data-suggest names every tested profile, for the
    chip check's "you need an ESP32-S3 such as ..." line."""
    table = boards.checked(boards.load())
    builds = [{"label": label, "manifest": f"firmware/{b['site_id']}/{app_set}/manifest.json",
               "board": b["short_name"], "profile": b["site_id"], "edition": app_set, "tag_id": b["tag_id"], "flash_mb": b["flash_mb"], "psram_mb": b["psram_mb"],
               "tested": b["tested"]} for b in (table["boards"][bid] for bid in boards.ids(table))]
    builds.append({"label": label, "manifest": None, "board": "Another ESP32-S3 board", "tag_id": None})
    # Tested boards only: an untested profile isn't one to send people to buy.
    suggest = [b["short_name"] for b in table["boards"].values() if b["tested"]]
    return (f'<div class="install" data-builds="{html.escape(json.dumps(builds))}" '
            f'data-suggest="{html.escape(json.dumps(suggest))}" data-help="#no-port">\n'
            '      <p class="install-note">Installing needs Chrome or Edge on a computer, with JavaScript on.</p>\n    </div>')


# The way to the firmware library (site/library/, site/installer/library_area.js), under "Pop in a new instrument".
ADD_BLOCK = """<a class="addfw addfw-link" id="add" href="library/"><span>Got Outrigger installed?</span> <b>Add custom FM-1 firmwares in the library</b></a>"""


BUILDERS = [
    ('Many boards, one firmware',
     'Each board is a profile: its pins, screen and touch. Screens, installer checks and tests adapt to it. Two tested today. <a href="https://github.com/chadthornton/outrigger/blob/master/docs/boards.md">Board list</a>'),
    ('Only the parts you need',
     'A shared base with apps on top, built into editions: Hub, or Hub + Loader. A general MIDI hub for any USB synth is planned.'),
    ('Open source',
     'GPL-3.0, core and apps. Build your own edition with only what you need. <a href="https://github.com/chadthornton/outrigger">Source on GitHub</a>'),
]
BUILDERS_HTML = '<div class="coming">' + "".join(f"<article><h4>{t}</h4><p>{d}</p></article>" for t, d in BUILDERS) + "</div>"



# The "For builders" credits, the footer, and the script that sends "Request support here" to the installer's dialog.
CREDITS = """  <dl class="credits">
    <div><dt>Installs with</dt><dd>
      <p><a href="https://github.com/espressif/esptool-js">esptool-js</a> 0.7.0 (Apache-2.0), served from this site.</p>
      <p>It writes only the program and its startup parts, never your settings or the backup store.</p></dd></div>
    <div><dt>Licenses</dt><dd>
      <p>Outrigger is <a href="LICENSE.txt">GPL-3.0</a>. The firmware includes LVGL and EspUsbHost (MIT), Arduino_GFX (BSD)
        and the Arduino ESP32 core (LGPL-2.1). <a href="NOTICE.txt">Licenses and credits</a></p></dd></div>
    <div><dt>Built on</dt><dd><ul>
      <li><a href="https://github.com/Quixotic7/MvaveFM1Unbricker">MvaveFM1Unbricker</a> <span>Quixotic7. Its FM-1 install handshake and package parsing, ported</span></li>
      <li><a href="https://github.com/kagaimiq/jl-uboot-tool">jl-uboot-tool</a> <span>kagaimiq. The FM-1 chip's boot commands and cipher, ported</span></li>
      <li><a href="https://github.com/hugelton/felucca">Felucca</a> <span>Hügelton Instruments. The firmware package format, by way of the Unbricker</span></li>
    </ul></dd></div>
    <div><dt>Thanks to</dt><dd><ul>
      <li><a href="https://github.com/charlesvestal/fm1-base">fm1-base</a>, <a href="https://github.com/charlesvestal/fm1-x0x">X0X</a>, <a href="https://github.com/charlesvestal/fm1-amb">AMB-1</a> and more firmwares <span><a href="https://github.com/charlesvestal">Charles Vestal</a></span></li>
      <li><a href="https://github.com/Quixotic7/RemoteFM1">RemoteFM1</a> <span>Quixotic7</span></li>
      <li><a href="https://github.com/hugelton/felucca-bsp">felucca-bsp</a> <span>Hügelton Instruments</span></li>
    </ul></dd></div>
    <div><dt>Related</dt><dd><ul>
      <li><a href="https://github.com/konsumer/fm1buddy">fm1buddy</a> <span>David Konsumer</span></li>
    </ul></dd></div>
  </dl>"""
FOOTER = """<footer class="nerd"><div class="wrap"><div class="fine">
  <p>Firmware screenshots by their authors, via the Firmware Hub. Not affiliated with M-VAVE or Waveshare.</p>
  <p id="installer-version" hidden></p>
</div></div></footer>"""
REQUEST_JS = """<script>
  // "Request support here" opens the installer's own request dialog; without one (no Chrome/Edge) it scrolls to the installer.
  for (const a of document.querySelectorAll("[data-request]")) a.addEventListener("click", (e) => {
    const req = [...document.querySelectorAll("#install .install-links a")].find((l) => l.textContent.startsWith("Request support"));
    if (req) { e.preventDefault(); req.click(); }
  });
</script>"""

real = MOCK
for a, b in [
    (INSTALL_BTN.format(out="out-b"), install_area("Check my ESP32", "hub-loader")),
    ('more at the Firmware Hub ↗</a></div>', 'more at the Firmware Hub ↗</a></div>\n  ' + ADD_BLOCK),
    ('<title>Outrigger: B3</title>', '<title>Outrigger: FM-1\'s little companion</title>\n'
     '<meta name="description" content="Play the M-VAVE FM-1 from any MIDI controller with a USB connection, and switch between 100+ firmwares without a laptop.">\n'
     '<script type="module" src="installer/area.js"></script>'),
    ('<link rel="stylesheet" href="shared.css">', '<link rel="stylesheet" href="style.css">'),
    ('<a class="link" href="#trouble">Help</a>',
     '<a class="link" href="library/">Firmware library</a><a class="link" href="#trouble">Help</a><a class="link" href="beta/">Beta testers</a>'),
    ('<footer><div class="wrap fine">Firmware screenshots by their authors, via the Firmware Hub. Not affiliated with M-VAVE or Waveshare.</div></footer>',
     f"""<section class="nerd" id="builders"><div class="wrap">
  <p class="eyebrow">For builders</p>
  <h2>Under the hood</h2>
  {BUILDERS_HTML}
{CREDITS}
</div></section>
{FOOTER}"""),
    ('<script src="rig.js"></script>', '<script src="rig.js"></script>\n' + REQUEST_JS),
]:
    assert a in real, a[:70]
    real = real.replace(a, b, 1)
(SITE / "index.html").write_text(real)
for f in ["art.svg", "rig.js"]:
    (SITE / f).write_text((HERE / f).read_text())
(SITE / "style.css").write_text((HERE / "shared.css").read_text())
for sub in ["shots", "shots/fw", "fonts"]:
    (SITE / sub).mkdir(parents=True, exist_ok=True)
for src in [*HERE.glob("shots/*.png"), *HERE.glob("shots/fw/*.png"), *HERE.glob("fonts/*.woff2")]:
    (SITE / src.relative_to(HERE)).write_bytes(src.read_bytes())
print("wrote site/index.html and its assets")

# Cloudflare Pages redirects (developers.cloudflare.com/pages/configuration/redirects/).
redirects = SITE / "_redirects"
if CHAT_INVITE:
    redirects.write_text(f"/chat {CHAT_INVITE} 302\n")
    print("wrote site/_redirects")
else:
    redirects.unlink(missing_ok=True)

# The beta testers' guide, outrigger.music/beta/, from docs/beta.
from guide import write_guide  # noqa: E402
write_guide(HERE.parent / "beta", SITE / "beta" / "index.html")
print("wrote site/beta/index.html")
