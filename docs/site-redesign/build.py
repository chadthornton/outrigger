#!/usr/bin/env python3
"""Writes the round-3 mock-up, b3-sidebar.html (direction B, Chad's pick on 2026-10-31).

    python3 docs/site-redesign/build.py

Round 2's generator (A2 and B2 from shared pieces) is in git history; its output files stay as they were.
Wording: "MIDI controller" ("with a USB connection" where it matters), never "keyboard" on its own (Chad: it reads as a typing keyboard). The user's
device is "your ESP32" before install and "Outrigger" after, never "the board".
"""
from pathlib import Path

HERE = Path(__file__).resolve().parent

BUY_BOARD = "https://www.amazon.com/s?k=Waveshare+ESP32-S3-Touch-LCD-1.69"
BUY_AMOLED = "https://www.amazon.com/gp/product/B0DSVK5576"
BUY_43B = "https://www.amazon.com/Waveshare-Capacitive-Development-Resolution-Dual-Core/dp/B0D925SBYF"
BUY_43B_CASE = "https://www.amazon.com/gp/product/B0DCNSRT31"
BUY_FM1_ALI = "https://www.aliexpress.us/w/wholesale-M-Vave-Fm1.html"
BUY_HUB = "https://www.amazon.com/Aceele-Delivery-Multiport-Splitter-MacBook/dp/B0CQJYM5L6"
BUY_FM1 = "https://www.amazon.com/M-VAVE-Synthesizer-Operators-Algorithms-Arpeggiator/dp/B0H5VCT7XK"
HUB_SITE = "https://thefirmwarehub.com/"
FEEDBACK = "mailto:feedback@outrigger.music"

# FM-1 screenshots the Firmware Hub hosts. They belong to each firmware's author: credit them, and ask before launch.
SCREENS = [
    ("sloop", "SLOOP", "a live groovebox"),
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
    <h3>Outrigger runs the show</h3>
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
    <p>Outrigger, the FM-1 and your MIDI controller all plug in here, and one charger powers the lot. The trick is a hub that
      can charge a laptop through it: Outrigger sits where the laptop would.</p>
    <svg viewBox="0 0 500 190" role="img" aria-label="Hub wiring: charger into the power input, Outrigger on the host port, FM-1 and controller on USB-C data ports"><use href="art.svg#hub-wiring"/></svg>
    <div class="row"><a class="need-link" href="#hub">What to look for ↓</a><button class="close" type="button" data-reset>Back to the whole setup</button></div>
  </section>
  <section class="panel" data-panel="fm1">
    <h3>The FM-1</h3>
    <p>M-VAVE's $60 pocket FM synth, with 27 keys, a speaker and a battery. Its players have written more than a hundred
      alternative firmwares for it, which turn it into a groovebox, a sampler, a drum machine and more.</p>
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
<p>Any MIDI controller with a USB connection plays the FM-1: full-size keys, pads and knobs on a pocket synth. No laptop on the table, no
  MIDI adapters, one charger for everything.</p>"""

SWITCH = f"""<h2>Pop in a new instrument</h2>
<p>A groovebox tonight, a sampler tomorrow: switch between more than a hundred community firmwares in about a minute,
  right on Outrigger. It backs up before every switch, so your songs are waiting when you come back.</p>
{SCREEN_STRIP}"""

INSTALL_BTN = """<button class="btn" type="button" data-install="{out}">Install Outrigger</button>
<p class="install-out" id="{out}" role="status" hidden></p>"""

SETUP = f"""<ol class="steps3">
  <li><div><b>Install Outrigger on your ESP32</b><p>Plug your ESP32 into a computer and click Install. About two
    minutes, free.</p>
    {INSTALL_BTN.format(out="out-b")}</div></li>
  <li><div><b>Plug everything into the hub</b><p>From then on, everything happens on Outrigger's screen.</p></div></li>
</ol>
<details class="overview"><summary>Installation overview</summary>
  <dl>
    <dt>In your browser</dt>
    <dd>Chrome and Edge can talk to a USB device directly. Install writes Outrigger onto your ESP32's own memory, then
      restarts it. Nothing goes near your FM-1, and your backups are never touched.</dd>
    <dt>Firmware switching</dt>
    <dd>Switching needs a 24 KB file from the FM-1's chip maker that we can't host ourselves. The installer fetches it
      from the open-source tool that already publishes it, checks it, and stores it on your ESP32.</dd>
    <dt>On the hub</dt>
    <dd>Outrigger runs on power from the hub's host port. It finds the FM-1 and your MIDI controller by itself, and its
      Play screen turns green when notes are getting through.</dd>
    <dt>Switching firmware</dt>
    <dd>Pick one on Outrigger's FM-1 screen and hold to confirm. It backs up the FM-1, writes the new firmware, checks
      every byte and restarts it, in about a minute.</dd>
    <dt>Updating later</dt>
    <dd>Put Outrigger in Computer mode from its Settings, plug it into a computer, and click Install again. Your
      backups stay.</dd>
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
      <p>This runs Outrigger. Most ESP32-S3 boards with a screen should work; we're still testing.</p>
      <ul class="picks">
        {pick("Tested", BUY_BOARD, "Waveshare ESP32-S3 Touch LCD 1.69 ↗")}
        {pick("Likely", BUY_AMOLED, "Waveshare ESP32-S3 1.8-inch AMOLED ↗")}
        {pick("Likely", BUY_43B, "Waveshare ESP32-S3 4.3-inch Touch, Board B ↗")}
      </ul>
      <p style="margin-top:8px"><b>Get the V2 of the 1.69</b>: it has a small "V2" sticker on the USB port. The first revision works too, but
        some tick audibly, about once a second.</p>
      <details class="nitpick"><summary>The nitpicky part: picking the right ESP32</summary>
        <ul>
          <li><b>ESP32-S3 or ESP32-P4.</b> A plain ESP32 can't connect USB devices.</li>
          <li><b>Native USB.</b> The USB-C port goes straight to the chip. Listings say "native USB" or "OTG".</li>
          <li><b>No "one-click download".</b> Those boards route USB through a converter chip that can't host.</li>
          <li><b>A TF card slot is worth having</b> for a coming version. The tested board doesn't have one.</li>
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
      <p>Most hubs that can charge a laptop through them work. Outrigger plugs in where the laptop would.</p>
      <ul class="picks">
        {pick("Likely", BUY_HUB, "Aceele 4-Port USB-C Hub with 100W PD ↗")}
      </ul>
      <details class="nitpick"><summary>The nitpicky part: picking the right hub</summary>
        <ul>
          <li><b>A charging input</b>, marked "PD in" or "pass-through". A 30 W USB-C charger is plenty.</li>
          <li><b>Host charging.</b> The hub powers the laptop port; that's what Outrigger runs on.</li>
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
      <p>M-VAVE's pocket FM synth, about $60–70.</p>
      <ul class="picks">
        {pick("Buy", BUY_FM1, "Amazon ↗")}
        {pick("Buy", BUY_FM1_ALI, "AliExpress ↗")}
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
    battery. Its players write their own firmware for it, which is what Outrigger switches between.</p></details>
  <details><summary>What does "firmware" mean here?</summary><p>The software the FM-1 runs. Swap it and the same
    hardware becomes a different instrument: a groovebox, a sampler, a chord machine. Browse them at
    <a href="https://thefirmwarehub.com/" target="_blank" rel="noopener">the Firmware Hub ↗</a>.</p></details>
  <details><summary>My MIDI controller has a 5-pin or 3.5 mm MIDI out</summary><p>Then a MIDI cable into the FM-1's MIDI IN
    plays it, no Outrigger needed. You'd still want Outrigger to switch firmware.</p></details>
  <details><summary>Which MIDI controllers work?</summary><p>USB controllers that work without a driver, which is most of
    them. One at a time for now: see What's coming.</p></details>
</div>"""

# What's coming, in three groups: (heading, one-line intro, cards). Each card: title, text, stage label, stage class.
COMING = [
    ("Over Wi-Fi", "Set up, update and check on Outrigger from your phone or laptop, without plugging it in.", [
        ("Wi-Fi", "Join your network from Outrigger's screen, so updates, downloads and its web page need no cable.", "In testing", "test"),
        ("outrigger.local", "Open Outrigger from any browser on your Wi-Fi: status, backups and settings.", "In testing", "test"),
        ("Firmware over Wi-Fi", "Browse the Firmware Hub on Outrigger's screen and download straight to Outrigger.", "Planned", "plan"),
        ("Backups on your computer", "Copy any backup off Outrigger, and restore the one you choose.", "Planned", "plan"),
        ("Troubleshoot from your browser", "Outrigger's page shows what it sees: the devices, its log and any crash. A fix starts with a link, not a guess.", "In testing", "test"),
    ]),
    ("Playing and recording", "More of your rig on one hub, and a way to capture what you play.", [
        ("Record the FM-1", "Capture its audio over USB at 44.1 kHz stereo and pull the takes off over Wi-Fi. Streaming comes after.", "In testing", "test"),
        ("A general-purpose MIDI hub", "Route any MIDI controller with a USB connection to any USB synth, not just the FM-1. The router is built; pointing it at other synths is a small change we haven't made or tested yet.", "Planned", "plan"),
        ("More devices at once", "Several controllers together, Bluetooth MIDI, and twice the USB room on ESP32-P4 boards.", "Planned", "plan"),
        ("The whole catalog on a TF card", 'Boards with a card slot keep every firmware locally, about 60 MB in all.<span class="tip"><b>Pro tip:</b> buying an ESP32 now? Get one with a TF card slot, like the <a href="#board">1.8-inch AMOLED</a>.</span>', "Planned", "plan"),
    ]),
    ("For builders", "Outrigger is meant to be extended.", [
        ("Write your own apps", "Outrigger is one core with plug-in apps. The core hands out USB, Wi-Fi and storage, and every screen renders and tests on a laptop. Next: a documented way for anyone to add an app.", "In testing", "test"),
        ("More ESP32 boards", "Support for a wide range of ESP32-S3 and ESP32-P4 boards: round and square screens from 1.69 to 4.3 inches, touch or buttons, with or without a card slot. Each gets a profile, and the screens already lay out at three sizes.", "In testing", "test"),
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
      Computer mode and hold the button. Then plug Outrigger into your computer and click Check my board, then Install. It goes back to
      MIDI by itself afterwards, and your backups stay.</p>
    <div class="shots-sm">
      <figure><img src="shots/M_setup.png" alt="Outrigger's Settings, with Computer mode: for updates over USB"><figcaption>Settings</figcaption></figure>
      <figure><img src="shots/M_computer_confirm.png" alt="Computer mode screen: hold to restart in Computer mode"><figcaption>Hold to restart</figcaption></figure>
    </div></div></details>
  <details id="no-port"><summary>Why can't the installer find my ESP32?</summary><p>Try another cable: charge-only cables never show up. Or
    hold BOOT, tap RST, let go of BOOT, and click Check my board again.</p></details>
  <details><summary>My FM-1 won't start. Is it broken?</summary><p>It's almost certainly not dead: its install mode lives in ROM.
    <a href="https://thefirmwarehub.com/#rescue" target="_blank" rel="noopener">The Firmware Hub's Rescue</a> or
    <a href="https://github.com/Quixotic7/MvaveFM1Unbricker" target="_blank" rel="noopener">the Unbricker</a> brings it back
    from a computer.</p></details>
  <details><summary>Why doesn't my second MIDI controller play?</summary><p>For now, Outrigger has room
    for one MIDI controller next to the FM-1. Unplug the other one. More than one is coming.</p></details>
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
  .eyebrow { font-size: .75rem; letter-spacing: .08em; text-transform: uppercase; color: var(--muted); font-weight: 600; }

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
  .fwshots figcaption b { color: var(--fg); display: block; font-weight: 600; }
  .fwmore { display: grid; place-content: center; text-align: center; gap: 2px; aspect-ratio: 1; border-radius: 8px; background: var(--soft);
            text-decoration: none; font-size: .8125rem; line-height: 1.3; color: var(--muted); padding: 8px; }
  .fwmore b { font-size: 1.375rem; font-weight: 650; color: var(--fg); }
  @media (max-width: 560px) { .fwshots { grid-template-columns: repeat(3, minmax(0, 1fr)); } }

  aside.rail { position: sticky; top: 24px; display: grid; gap: 16px; max-height: calc(100vh - 48px); overflow-y: auto; overscroll-behavior: contain; scrollbar-width: none; }
  @media (max-width: 920px) { aside.rail { position: static; max-height: none; overflow: visible; } }
  .card { background: var(--panel); border: 1px solid var(--line); border-radius: var(--radius); padding: 24px; display: grid; gap: 16px; }
  .steps3 { list-style: none; margin: 0; padding: 0; counter-reset: s; display: grid; gap: 20px; }
  .steps3 li { counter-increment: s; display: grid; grid-template-columns: 28px minmax(0, 1fr); gap: 12px; align-items: start; }
  .steps3 li::before { content: counter(s); width: 24px; height: 24px; margin-top: 1px; border-radius: 50%; background: var(--accent);
                       color: var(--accent-fg); display: grid; place-items: center; font-weight: 650; font-size: .75rem; }
  .steps3 b { display: block; font-weight: 600; line-height: 1.35; margin-bottom: 4px; text-wrap: balance; }
  .steps3 p { font-size: .9375rem; line-height: 1.5; color: var(--muted); }
  .steps3 .btn { margin-top: 14px; width: 100%; justify-content: center; }
  .steps3 .install-out { margin-top: 10px; font-size: .875rem; line-height: 1.45; }
  .shopping { list-style: none; margin: 0; padding: 0; display: grid; }
  .shopping a { display: grid; grid-template-columns: 36px 1fr auto; gap: 14px; align-items: center; padding: 12px 0; text-decoration: none; border-bottom: 1px solid var(--line); }
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
  .likely-h { margin-top: 20px !important; font-size: .9375rem; }
  .nitpick ul.likely { list-style: none; padding: 0; margin-top: 8px; gap: 6px; }
  .likely li { display: flex; justify-content: space-between; gap: 12px; align-items: baseline; }
  .likely a { font-weight: 550; color: var(--fg); }
  .likely small { color: var(--muted); white-space: nowrap; font-size: .8125rem; }

  .coming { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 16px; }
  @media (max-width: 860px) { .coming { grid-template-columns: repeat(2, minmax(0, 1fr)); } }
  @media (max-width: 560px) { .coming { grid-template-columns: 1fr; } }
  .coming article { background: var(--panel); border: 1px solid var(--line); border-radius: var(--radius); padding: 24px; display: grid; gap: 8px; align-content: start; }
  .coming .stage { display: flex; align-items: center; gap: 8px; font-size: .75rem; letter-spacing: .06em; text-transform: uppercase; font-weight: 600; color: var(--muted); margin-bottom: 4px; }
  .coming .stage::before { content: ""; width: 7px; height: 7px; border-radius: 50%; background: var(--dot, var(--line)); }
  .coming .s-test { --dot: var(--accent); } .coming .s-plan { --dot: var(--muted); }
  .coming h4 { margin: 0; font-size: 1.0625rem; line-height: 1.35; font-weight: 650; text-wrap: balance; }
  .cgroup + .cgroup { margin-top: 48px; }
  .faq .answer { margin: -4px 0 18px; display: grid; gap: 14px; }
  .faq .answer p { color: var(--muted); max-width: 38em; }
  .shots-sm { display: flex; gap: 14px; }
  .shots-sm figure { margin: 0; display: grid; gap: 6px; justify-items: center; }
  .shots-sm img { width: 120px; border-radius: 18px; border: 3px solid var(--fg); background: #000; }
  .shots-sm figcaption { font-size: .8125rem; color: var(--muted); }
  .cgroup-head { display: flex; align-items: baseline; gap: 12px; flex-wrap: wrap; margin-bottom: 16px; }
  .cgroup-head h3 { font-size: 1.125rem; line-height: 1.3; }
  .cgroup-head p { color: var(--muted); font-size: .9375rem; }
  .coming p { color: var(--muted); font-size: .9375rem; line-height: 1.55; }
  .coming .tip { display: block; margin-top: 10px; padding: 10px 12px; border-radius: 8px; background: color-mix(in srgb, var(--accent) 10%, transparent); color: var(--fg); font-size: .875rem; line-height: 1.45; }
  .coming .tip b { font-weight: 650; }
  /* The real install area (site/installer/area.js, from PR #14): buttons, then the board readout, progress and result. */
  .install { display: grid; gap: 10px; margin-top: 14px; }
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
  .install-log { border: 0; padding: 0; font-size: .8125rem; color: var(--muted); }
  .install-log pre { margin: 6px 0 0; max-height: 14em; overflow: auto; white-space: pre-wrap; font: .75rem ui-monospace, "SF Mono", Menlo, monospace; }
  .install .check { display: flex; gap: 8px; align-items: baseline; }
  .install progress { width: 100%; accent-color: var(--accent); }
  footer .install { margin: 8px 0 12px; max-width: 320px; }
  footer .install button { padding: 8px 14px; font-size: .875rem; width: auto; }
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
  <a class="link wide" href="#need">What you need</a><a class="link wide" href="#coming">What's coming</a><a class="link" href="#trouble">Help</a>
</nav>
<div class="wrap">
  <div class="layout">
    <main>
      <section class="hero">
        <h1>FM&#8209;1&rsquo;s new best friend</h1>
        <p class="sub"><span>Plug in your keys.</span> <span>Pop in a new instrument.</span></p>
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
      <aside><p class="eyebrow">Before you buy</p>{BEFORE}</aside>
    </div>
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
FW = "firmware/waveshare-esp32-s3-touch-lcd-1.69"


def install_area(label, app_set):
    builds = f'[{{"label":"{label}","manifest":"{FW}/{app_set}/manifest.json"}}]'
    return (f"<div class=\"install\" data-builds='{builds}' data-help=\"#no-port\">\n"
            '      <p class="install-note">Installing needs Chrome or Edge on a computer, with JavaScript on.</p>\n    </div>')


REAL_STEP2 = """<li><div><b>Three firmwares come with it</b><p>On a new ESP32 the installer also puts M-VAVE stock V15, Felucca
    and Skiff on Outrigger, downloaded from the Firmware Hub straight into your browser. Add more from the Hub's list
    any time: see <a href="#add">Add a firmware</a>.</p></div></li>
  <li><div><b>Plug everything into the hub</b>"""

# Adding a Firmware Hub firmware to a set-up Outrigger (site/installer/add_area.js), its own section under the hero.
ADD_SECTION = """<section class="section" id="add" style="max-width:760px"><h2>Add a firmware</h2>
    <p>Put any firmware from the Firmware Hub's list on Outrigger, up to ten in all. Your browser downloads it from
      thefirmwarehub.com and checks it, and Outrigger checks it again before storing it. Nothing else on Outrigger
      changes: your backups and the firmwares already there stay as they are. Switching to a firmware we haven't
      tested with yours clears the old one's data first, and keeps a copy that comes back when you switch back.</p>
    <div class="install add-firmware"><p class="install-note">Adding a firmware needs Chrome or Edge on a computer, with JavaScript on.</p></div>
  </section>
  <section class="section" id="need">"""

real = MOCK
for a, b in [
    (INSTALL_BTN.format(out="out-b"), install_area("Check my board", "hub-loader")),
    ("""Plug your ESP32 into a computer and click Install. About two
    minutes, free.</p>""", """Plug your ESP32 into a computer and click Check my board: it
    says what's on the board and changes nothing. Then click Install. About two minutes, free.</p>"""),
    ('<li><div><b>Plug everything into the hub</b>', REAL_STEP2),
    ("""<dd>Switching needs a 24 KB file from the FM-1's chip maker that we can't host ourselves. The installer fetches it
      from the open-source tool that already publishes it, checks it, and stores it on your ESP32.</dd>""",
     """<dd>Switching needs a 24 KB file from the FM-1's chip maker that we can't host ourselves. On a new ESP32 the
      installer downloads it straight from GitHub into your browser, and the three starter firmwares from the Firmware
      Hub, checks every one, and puts them on Outrigger. If a download fails, nothing is written.</dd>"""),
    ('<section class="section" id="need">', ADD_SECTION),
    ('<title>Outrigger: B3</title>', '<title>Outrigger: FM-1\u2019s new best friend</title>\n'
     '<meta name="description" content="Play the M-VAVE FM-1 from any MIDI controller with a USB connection, and switch between 100+ firmwares without a laptop.">\n'
     '<script type="module" src="installer/area.js"></script>\n<script type="module" src="installer/add_area.js"></script>'),
    ('<link rel="stylesheet" href="shared.css">', '<link rel="stylesheet" href="style.css">'),
    ('<a class="link" href="#trouble">Help</a>',
     '<a class="link" href="#trouble">Help</a><a class="link" href="beta/">Beta testers</a>'),
    ('<footer><div class="wrap fine">Firmware screenshots by their authors, via the Firmware Hub. Not affiliated with M-VAVE or Waveshare.</div></footer>',
     f"""<footer><div class="wrap fine">
  <p>For developers: Everything adds a Wi-Fi status page; its loader can't write.</p>
  {install_area("Check my board (Everything)", "everything")}
  <p>Installs with <a href="https://github.com/espressif/esptool-js">esptool-js</a> 0.7.0 (Apache-2.0), served from this site.
    It writes only the program and its startup parts, never your settings or the backup store. Outrigger is
    <a href="LICENSE.txt">GPL-3.0</a> (<a href="https://github.com/chadthornton/outrigger">source</a>); the firmware includes
    LVGL and EspUsbHost (MIT), Arduino_GFX (BSD) and the Arduino ESP32 core (LGPL-2.1):
    <a href="NOTICE.txt">licenses and credits</a>. Firmware screenshots by their authors, via the Firmware Hub. Not affiliated with M-VAVE or
    Waveshare.</p>
</div></footer>"""),
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

# The beta testers' guide, outrigger.music/beta/, from docs/beta.
from guide import write_guide  # noqa: E402
write_guide(HERE.parent / "beta", SITE / "beta" / "index.html")
print("wrote site/beta/index.html")
