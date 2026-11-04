#!/usr/bin/env python3
"""Writes directions.html: the homepage hero, "Set up once" and one feature, rendered in the current look and three
alternative visual directions, for Chad to compare (2026-11-04, before the beta announcement).

    python3 docs/site-redesign/directions/build.py

Exploration only; nothing here touches site/. Copy is taken from the live site as of 2026-11-04 and is not a proposal:
the orchestrator audits copy separately. Small mono labels some directions add (spec rows, figure captions) are a
visual device built only from facts already on the page.
"""
import base64
import re
from pathlib import Path

HERE = Path(__file__).resolve().parent
SITE = HERE.parent.parent.parent / "site"
OUT = HERE / "directions.html"

FONTS = ("https://fonts.googleapis.com/css2?family=Commissioner:wght@400..700"
         "&family=Archivo:wdth,wght@62..125,400..800&family=Martian+Mono:wdth,wght@75..112.5,400..600"
         "&family=Atkinson+Hyperlegible+Next:wght@400..700&family=Atkinson+Hyperlegible+Mono:wght@400..700"
         "&family=Geist+Mono:wght@400..600&display=swap")

TOKENS = ["bg", "fg", "muted", "line", "panel", "soft", "screen", "accent", "accent-fg", "warn", "grid"]

# Each direction: light and dark token values, and the notes shown above its render.
DIRS = [
    {
        "id": "current", "cls": "d-base", "tab": "Current", "name": "Current site",
        "light": dict(bg="#f6f5f1", fg="#151714", muted="#5f645c", line="#dddfd7", panel="#ffffff", soft="#eceee6",
                      screen="#141713", accent="#1f8a4c", **{"accent-fg": "#ffffff"}, warn="#9a3a00", grid="transparent"),
        "dark": dict(bg="#0f110e", fg="#eceee8", muted="#9ba096", line="#2a2e28", panel="#181b17", soft="#1d211c",
                     screen="#050605", accent="#46d384", **{"accent-fg": "#06150c"}, warn="#ffab6b", grid="transparent"),
        "pair": "Commissioner 400–700 for everything",
        "scale": "13 · 15 · 17 · 22 · 24 sub · 52 h1 (clamp 30–52)",
        "body": "The baseline, ported rule for rule from style.css and the page's inline styles. Commissioner is a "
                "humanist sans with soft, slightly flared terminals; with the warm off-white, the 14px radius and "
                "the green numbered dots, it reads as friendly consumer hardware. That suits a first-time FM-1 "
                "owner. A firmware author sees a polished product page and has to dig to the footer to find the "
                "engineering.",
    },
    {
        "id": "datasheet", "cls": "d-sheet", "tab": "A · Datasheet", "name": "A · Datasheet",
        "light": dict(bg="#f2f2ef", fg="#111311", muted="#555954", line="#cfd1cb", panel="#fbfbf9", soft="#e7e8e3",
                      screen="#121412", accent="#c2410c", **{"accent-fg": "#ffffff"}, warn="#b42318", grid="transparent"),
        "dark": dict(bg="#0d0e0d", fg="#e8eae5", muted="#91958e", line="#2c2f2b", panel="#141614", soft="#1b1d1a",
                     screen="#000000", accent="#ff8a4c", **{"accent-fg": "#1a0800"}, warn="#ff8f80", grid="transparent"),
        "pair": "Archivo (variable width, set at 100 and 112) + Martian Mono at 87.5 width for silkscreen labels",
        "scale": "11 label · 13 · 16 body · 20 · 26 · 56 h1 (clamp 34–56)",
        "body": "Borrows from synth back-panels and the Espressif datasheets these people already read: a working "
                "grotesk, caps labels like silkscreen, a heavy rule over each section, figure captions, and spec "
                "rows with units. Square buttons read as hardware keys. To a firmware community it says someone "
                "measured things. Trade-offs: it is the coldest of the three, and “new best friend” "
                "sits oddly in a spec sheet. The orange is an LED colour, which means leaving brand green behind.",
    },
    {
        "id": "notebook", "cls": "d-note", "tab": "B · Lab notebook", "name": "B · Lab notebook",
        "light": dict(bg="#eef2f1", fg="#18211f", muted="#53605c", line="#c6d0cd", panel="#fbfcfc", soft="#e2e9e7",
                      screen="#0f1514", accent="#1d6b8f", **{"accent-fg": "#ffffff"}, warn="#9a3a00", grid="#dce4e2"),
        "dark": dict(bg="#0e1413", fg="#dbe4e1", muted="#8b9995", line="#283432", panel="#131b1a", soft="#18211f",
                     screen="#050807", accent="#6cb6dc", **{"accent-fg": "#06131b"}, warn="#ffab6b", grid="#162020"),
        "pair": "Atkinson Hyperlegible Mono for headings, steps and labels + Atkinson Hyperlegible Next for running text",
        "scale": "12 label · 14 · 17 body · 19 · 24 · 44 h1 (clamp 28–44)",
        "body": "Mono headings on an engineering-pad grid, with steps as a checklist you tick off. It looks like a "
                "README and a build log, which is where this audience lives, and it promises you can follow along. "
                "The two Atkinson faces share bones, so mono headings and proportional text don't clash. "
                "Trade-offs: mono headlines run wide, so the h1 takes three lines on a phone. The grid adds noise "
                "behind screenshots. It risks reading as a weekend project rather than a finished tool.",
    },
    {
        "id": "instrumented", "cls": "d-inst", "tab": "C · Commissioner, instrumented", "name": "C · Commissioner, instrumented",
        "light": dict(bg="#f4f5f1", fg="#131512", muted="#5b6059", line="#d9dbd3", panel="#ffffff", soft="#ebece6",
                      screen="#131612", accent="#1f8a4c", **{"accent-fg": "#ffffff"}, warn="#9a3a00", grid="transparent"),
        "dark": dict(bg="#0e100d", fg="#eceee8", muted="#9aa095", line="#272b25", panel="#161915", soft="#1c1f1a",
                     screen="#050605", accent="#46d384", **{"accent-fg": "#06150c"}, warn="#ffab6b", grid="transparent"),
        "pair": "Commissioner 400–700 for words + Geist Mono 400–500 for numbers, units and step markers only",
        "scale": "12 mono · 15 · 17 body · 21 · 22 sub · 50 h1 (clamp 30–50)",
        "body": "Keeps Commissioner, the green and the warm voice. It cuts the radius from 14 to 8, swaps filled "
                "dots for hairline mono markers, and adds a readout strip for the numbers this crowd checks first: "
                "about two minutes to install, about a minute per switch, 100+ firmwares, and that it's free. The words stay humanist and the facts get instrument type. Trade-offs: it is the smallest "
                "change, so it makes less of a splash on announcement day. Two faces need discipline: once mono "
                "leaks into sentences, it turns into B.",
    },
]

RECOMMEND = [
    "Ship C. It keeps the Commissioner voice you like and makes the facts look measured.",
    "Mono is reserved for numbers, units and step markers, so the page reads as more technical without getting cold.",
    "It's also the only one small enough to land as a CSS pass on style.css without touching the copy audit.",
]


def theme_css(sel, d):
    """Light tokens on the scope; dark ones for a dark host (system or explicit) and for the frame's own override."""
    light = "".join(f"--{k}:{d['light'][k]};" for k in TOKENS)
    dark = "".join(f"--{k}:{d['dark'][k]};" for k in TOKENS) + "color-scheme:dark;"
    return (f"{sel}{{{light}color-scheme:light}}\n"
            f"@media (prefers-color-scheme: dark){{:root:not([data-theme=\"light\"]) {sel}:not([data-scheme=\"light\"]){{{dark}}}}}\n"
            f":root[data-theme=\"dark\"] {sel}:not([data-scheme=\"light\"]),{sel}[data-scheme=\"dark\"]{{{dark}}}\n")


def sprite():
    svg = (SITE / "art.svg").read_text()
    inner = re.sub(r"^\s*<svg[^>]*>|</svg>\s*$", "", svg.strip())
    return f'<svg width="0" height="0" style="position:absolute" aria-hidden="true">{inner}</svg>'


def png(name):
    return "data:image/png;base64," + base64.b64encode((SITE / "shots" / "fw" / f"{name}.png").read_bytes()).decode()


SCREENS = [("sloop", "SLOOP", "a live groovebox"), ("x0x", "X0X", "909, 808 and two 303s"),
           ("zp12", "zp12", "a 12-bit sampler"), ("winampfm", "winampFM", "a synth that looks like Winamp")]


def page(d):
    """The same homepage content for every direction; `x` holds the few labels a direction adds as visual devices."""
    k = d["id"]
    x = {
        "current": {},
        "datasheet": {
            "meta": '<div class="meta"><span>USB MIDI host for the M-VAVE FM-1</span><span>ESP32-S3 · P4</span></div>',
            "fig": '<figcaption><b>Fig. 1</b> The setup. MIDI controller and FM-1 on a powered USB-C hub; Outrigger on the host port.</figcaption>',
            "tag1": "MIDI controller", "tag2": "Firmware",
            "spec": [("Firmwares", "100+"), ("Switch", "≈ 1 min"), ("Backup", "before every switch")],
            "steps": ["≈ 2 min · free", "3 included", "1 charger"],
        },
        "notebook": {
            "fig": '<figcaption>fig. the setup — controller and FM-1 on one hub, Outrigger on the host port</figcaption>',
            "tag1": "plug in", "tag2": "switch",
            "note": "Outrigger backs up the FM-1 before it writes anything.",
            "steps": ["install", "add", "plug in"],
        },
        "instrumented": {
            "readout": [("Install", "≈ 2", "min"), ("Switch", "≈ 1", "min"), ("Firmwares", "100+", ""), ("Outrigger", "free", "")],
            "steps": ["1/3", "2/3", "3/3"],
        },
    }[k]
    steps = x.get("steps", ["", "", ""])
    stepmark = lambda i: f'<span class="mk">{steps[i]}</span>' if steps[i] else ""
    shots = "".join(f'<figure><img src="{png(f)}" alt="{n} on the FM-1\'s screen"><figcaption><b>{n}</b> {c}</figcaption></figure>'
                    for f, n, c in SCREENS)
    spec = ""
    if "spec" in x:
        spec = '<dl class="spec">' + "".join(f"<div><dt>{a}</dt><dd>{b}</dd></div>" for a, b in x["spec"]) + "</dl>"
    readout = ""
    if "readout" in x:
        readout = '<dl class="readout">' + "".join(
            f'<div><dt>{a}</dt><dd><b>{b}</b>{(" " + u) if u else ""}</dd></div>' for a, b, u in x["readout"]) + "</dl>"
    note = f'<p class="margin-note">{x["note"]}</p>' if "note" in x else ""
    tag = lambda t: f'<p class="tag">{t}</p>' if t else ""
    return f"""
<nav class="topnav">
  <a class="wordmark" href="#{k}"><i></i>Outrigger</a><span class="beta">Beta</span>
  <span class="sp"></span>
  <a class="link wide" href="#{k}">What you need</a><a class="link wide" href="#{k}">What's coming</a><a class="link" href="#{k}">Help</a><a class="link" href="#{k}">Beta testers</a>
</nav>
<div class="layout">
  <main>
    <section class="hero">
      {x.get("meta", "")}
      <h1>FM&#8209;1&rsquo;s new best friend</h1>
      <p class="sub"><span>Plug in your keys.</span> <span>Pop in a new instrument.</span></p>
      {readout}
      <figure class="rig-fig">
        <svg viewBox="0 0 640 312" role="img" aria-label="A MIDI controller and the FM-1 plugged into a USB hub, with Outrigger on the hub"><use href="#rig"/></svg>
        {x.get("fig", "")}
      </figure>
    </section>
    <div class="features">
      <section class="feature">
        <svg class="ico" viewBox="0 0 220 90" aria-hidden="true"><use href="#keyboard"/></svg>
        <div>{tag(x.get("tag1"))}<h2>Plug in your keys</h2>
          <p>Any MIDI controller with a USB connection plays the FM-1: full-size keys, pads and knobs on a pocket synth. No laptop on the table, no
          MIDI adapters, one charger for everything.</p></div>
      </section>
      <section class="feature">
        <svg class="ico" viewBox="0 0 162 97" aria-hidden="true"><use href="#fm1"/></svg>
        <div>{tag(x.get("tag2"))}<h2>Pop in a new instrument</h2>
          <p>A groovebox tonight, a sampler tomorrow: switch between more than a hundred community firmwares in about a minute,
          right on Outrigger. It backs up before every switch, so your songs are waiting when you come back.</p>
          {note}{spec}
          <div class="fwshots">{shots}<a class="fwmore" href="https://thefirmwarehub.com/" target="_blank" rel="noopener"><b>100+</b> more at the Firmware Hub ↗</a></div>
        </div>
      </section>
    </div>
  </main>
  <aside class="rail">
    <div class="card"><p class="eyebrow">Set up once</p><ol class="steps3">
      <li>{stepmark(0)}<div><b>Install Outrigger on your ESP32</b><p>Plug your ESP32 into a computer and click Check my board: it
        says what's on the board and changes nothing. Then click Install. About two minutes, free.</p>
        <div class="install"><fieldset class="install-boards"><legend>Which ESP32 do you have?</legend>
          <label><input type="radio" name="b-{k}" id="b-{k}-1" checked> Waveshare 1.69"</label>
          <label><input type="radio" name="b-{k}" id="b-{k}-2"> Waveshare 3.5"</label>
          <label><input type="radio" name="b-{k}" id="b-{k}-3"> Another ESP32-S3 board</label></fieldset>
          <button type="button">Check my board</button>
          <p class="install-note">Checking reads the board and changes nothing: nothing is installed until you confirm. Installing replaces the program only. Settings, Wi-Fi and FM-1 backups stay. <a href="#{k}">No board listed?</a></p>
        </div></div></li>
      <li>{stepmark(1)}<div><b>Add firmwares</b><p>Stock V15, Felucca and Skiff come with it. Add more from the Firmware
        Hub's list, right on this page: <a href="#{k}">Add custom firmwares</a>.</p></div></li>
      <li>{stepmark(2)}<div><b>Plug everything into the hub</b><p>From then on, everything happens on Outrigger's screen.</p></div></li>
    </ol>
    <details class="overview"><summary>Installation overview</summary><p>Chrome and Edge can talk to a USB device directly. Install writes Outrigger onto your ESP32's own memory, then
      restarts it. Nothing goes near your FM-1, and your backups are never touched.</p></details></div>
  </aside>
</div>"""


# Rules every direction shares: structure only, no colour or type.
COMMON = """
.frame{container-type:inline-size;background:var(--bg);color:var(--fg);border:1px solid var(--shell-line);border-radius:12px;overflow:hidden;margin-inline:auto;transition:max-width .25s}
.frame[data-width="phone"]{max-width:390px}
.frame *{box-sizing:border-box}
.frame .site{padding:0 24px 48px;max-width:1120px;margin:0 auto}
.frame a{color:inherit;text-underline-offset:3px}
.frame h1,.frame h2,.frame h3{margin:0;text-wrap:balance}
.frame p{margin:0;text-wrap:pretty}
.frame img,.frame svg{display:block;max-width:100%}
.frame .topnav{display:flex;align-items:center;gap:24px;padding-block:20px}
.frame .topnav .sp{flex:1}
.frame .topnav .link{text-decoration:none;color:var(--muted)}
.frame .wordmark{display:inline-flex;gap:8px;align-items:center;text-decoration:none}
.frame .wordmark i{display:inline-block}
.frame .layout{display:grid;grid-template-columns:minmax(0,1fr) 312px;gap:64px;padding-block:48px 32px;align-items:start}
.frame .hero .sub span{white-space:nowrap}
.frame .rig-fig{margin:32px 0 0;max-width:540px}
.frame .rig-fig svg{width:100%;height:auto;color:var(--fg)}
.frame .features{margin-top:48px;display:grid}
.frame .feature{display:grid;grid-template-columns:72px minmax(0,1fr);gap:24px;padding-block:32px}
.frame .feature .ico{width:72px;color:var(--fg);margin-top:4px}
.frame .feature p{max-width:34em}
.frame .fwshots{margin-top:24px;display:grid;grid-template-columns:repeat(5,minmax(0,1fr));gap:16px 12px;max-width:620px}
.frame .fwshots figure{margin:0;display:grid;gap:8px;align-content:start}
.frame .fwshots img{width:100%;aspect-ratio:1;background:#000}
.frame .fwshots figcaption b{display:block;color:var(--fg)}
.frame .fwmore{display:grid;place-content:center;text-align:center;gap:2px;aspect-ratio:1;max-width:100%;text-decoration:none;padding:8px}
.frame .card{display:grid;gap:16px}
.frame .steps3{list-style:none;margin:0;padding:0;display:grid;gap:20px}
.frame .steps3 li{display:grid;grid-template-columns:28px minmax(0,1fr);gap:12px;align-items:start}
.frame .steps3 b{display:block;line-height:1.35;margin-bottom:4px}
.frame .install{display:grid;gap:10px;margin-top:14px}
.frame .install-boards{border:0;margin:0;padding:0;display:flex;flex-wrap:wrap;gap:6px 18px}
.frame .install-boards legend{padding:0;margin-bottom:6px}
.frame .install-boards label{display:flex;gap:6px;align-items:center;cursor:pointer}
.frame .install-boards input{accent-color:var(--accent);margin:0}
.frame .install button{font:inherit;cursor:pointer;width:100%;border:0;background:var(--accent);color:var(--accent-fg)}
.frame .install-note{color:var(--muted)}
.frame .overview{border-top:1px solid var(--line);padding-top:16px}
.frame .overview summary{cursor:pointer}
.frame .overview p{margin-top:10px;color:var(--muted)}
.frame :focus-visible{outline:3px solid var(--accent);outline-offset:3px}
@container (max-width:920px){.frame .layout{grid-template-columns:1fr;gap:48px;padding-top:32px}}
@container (max-width:560px){
  .frame .site{padding-inline:16px}
  .frame .topnav .wide{display:none}
  .frame .feature{grid-template-columns:1fr;gap:12px}.frame .feature .ico{width:56px}
  .frame .fwshots{grid-template-columns:repeat(3,minmax(0,1fr))}
}
"""

# The current site, ported from style.css and index.html's inline styles.
BASE = """
.d-base{font:400 1.0625rem/1.6 "Commissioner",system-ui,-apple-system,"Segoe UI",sans-serif;-webkit-font-smoothing:antialiased}
.d-base .topnav{border-bottom:1px solid var(--line)}
.d-base .topnav .link{font-size:.9375rem}
.d-base .wordmark{font-weight:700;letter-spacing:-.01em;font-size:1.1rem}
.d-base .wordmark i{width:10px;height:10px;border-radius:50%;background:var(--accent)}
.d-base .beta{font-size:.7rem;font-weight:700;letter-spacing:.08em;text-transform:uppercase;color:var(--muted);border:1px solid var(--line);border-radius:999px;padding:2px 8px}
.d-base h1{font-size:clamp(1.875rem,4.2cqi,3.25rem);line-height:1.08;letter-spacing:-.025em;font-weight:680;max-width:14em}
.d-base .sub{margin-top:12px;font-size:clamp(1.25rem,2.2cqi,1.5rem);line-height:1.35;color:var(--muted);font-weight:500;letter-spacing:-.01em}
.d-base .feature{border-top:1px solid var(--line)}
.d-base .feature h2{font-size:1.375rem;line-height:1.25;letter-spacing:-.01em;font-weight:650}
.d-base .feature p{margin-top:10px;color:var(--muted)}
.d-base .fwshots img{border-radius:8px;box-shadow:0 0 0 1px var(--line),0 1px 2px rgb(0 0 0/.12)}
.d-base .fwshots figcaption{font-size:.8125rem;color:var(--muted);line-height:1.35}
.d-base .fwshots figcaption b{font-weight:600}
.d-base .fwmore{border-radius:8px;background:var(--soft);font-size:.8125rem;line-height:1.3;color:var(--muted)}
.d-base .fwmore b{font-size:1.375rem;font-weight:650;color:var(--fg)}
.d-base .card{background:var(--panel);border:1px solid var(--line);border-radius:14px;padding:24px}
.d-base .eyebrow{font-size:.75rem;letter-spacing:.08em;text-transform:uppercase;color:var(--muted);font-weight:600}
.d-base .steps3{counter-reset:s}
.d-base .steps3 li{counter-increment:s}
.d-base .steps3 li::before{content:counter(s);width:24px;height:24px;margin-top:1px;border-radius:50%;background:var(--accent);color:var(--accent-fg);display:grid;place-items:center;font-weight:650;font-size:.75rem}
.d-base .steps3 b{font-weight:600}
.d-base .steps3 p{font-size:.9375rem;line-height:1.5;color:var(--muted)}
.d-base .install-boards{font-size:.9375rem}
.d-base .install-boards legend{font-weight:600;font-size:.875rem}
.d-base .install button{border-radius:10px;padding:13px 22px;font-weight:600;line-height:1.2}
.d-base .install-note{font-size:.875rem;line-height:1.45}
.d-base .overview summary{font-weight:550;font-size:.9375rem}
.d-base .overview p{font-size:.875rem;line-height:1.5}
"""

# A: datasheet / instrument panel. Square, ruled, captioned; caps mono labels as silkscreen.
SHEET = """
.d-sheet{font:400 1rem/1.55 "Archivo",system-ui,sans-serif;font-stretch:100%;-webkit-font-smoothing:antialiased}
.d-sheet .lbl,.d-sheet .tag,.d-sheet .eyebrow,.d-sheet .beta,.d-sheet .meta,.d-sheet .spec dt,.d-sheet .mk,.d-sheet .install-boards legend,.d-sheet .install button,.d-sheet figcaption b,.d-sheet .fwshots figcaption b,.d-sheet .topnav .link{font-family:"Martian Mono",ui-monospace,monospace;font-stretch:87.5%;text-transform:uppercase;letter-spacing:.06em;font-weight:500}
.d-sheet .topnav{border-bottom:2px solid var(--fg)}
.d-sheet .topnav .link{font-size:.6875rem}
.d-sheet .wordmark{font-weight:800;font-stretch:112%;font-size:1.125rem;letter-spacing:-.01em;text-transform:uppercase}
.d-sheet .wordmark i{width:8px;height:8px;background:var(--accent);box-shadow:0 0 0 3px color-mix(in srgb,var(--accent) 25%,transparent);border-radius:50%}
.d-sheet .beta{font-size:.625rem;border:1px solid var(--fg);padding:2px 6px}
.d-sheet .meta{display:flex;justify-content:space-between;flex-wrap:wrap;gap:4px 16px;font-size:.6875rem;color:var(--muted);border-bottom:1px solid var(--line);padding-bottom:10px;margin-bottom:28px}
.d-sheet h1{font-size:clamp(2.125rem,5.4cqi,3.5rem);line-height:1;letter-spacing:-.03em;font-weight:750;font-stretch:112%;max-width:12em}
.d-sheet .sub{margin-top:14px;font-size:clamp(1.125rem,2cqi,1.375rem);line-height:1.35;color:var(--muted);font-weight:450}
.d-sheet .rig-fig{border-top:1px solid var(--line);border-bottom:1px solid var(--line);padding-block:16px 12px}
.d-sheet .rig-fig figcaption{margin-top:12px;font-size:.8125rem;color:var(--muted);line-height:1.45}
.d-sheet .rig-fig figcaption b{font-size:.6875rem;color:var(--fg);margin-right:8px}
.d-sheet .feature{border-top:2px solid var(--fg);padding-top:20px}
.d-sheet .feature .ico{margin-top:22px}
.d-sheet .tag{font-size:.6875rem;color:var(--accent);margin-bottom:8px}
.d-sheet .feature h2{font-size:1.625rem;line-height:1.15;letter-spacing:-.02em;font-weight:700;font-stretch:112%}
.d-sheet .feature p{margin-top:10px;color:var(--muted)}
.d-sheet .spec{margin:20px 0 0;display:grid;grid-template-columns:repeat(3,minmax(0,1fr));border-top:1px solid var(--line);border-bottom:1px solid var(--line);max-width:620px}
.d-sheet .spec div{padding:10px 12px 10px 0;display:grid;gap:2px;min-width:0}
.d-sheet .spec div+div{border-left:1px solid var(--line);padding-left:12px}
.d-sheet .spec dt{font-size:.625rem;color:var(--muted)}
.d-sheet .spec dd{margin:0;font-size:.9375rem;font-weight:600;font-variant-numeric:tabular-nums}
.d-sheet .fwshots img{border-radius:0;outline:1px solid var(--fg);outline-offset:-1px}
.d-sheet .fwshots figcaption{font-size:.8125rem;color:var(--muted);line-height:1.35}
.d-sheet .fwshots figcaption b{font-size:.6875rem;margin-bottom:2px}
.d-sheet .fwmore{border:1px dashed var(--muted);font-size:.75rem;color:var(--muted)}
.d-sheet .fwmore b{font-size:1.5rem;font-weight:750;font-stretch:112%;color:var(--fg)}
.d-sheet .card{background:var(--panel);border:1px solid var(--fg);border-radius:0;padding:0;gap:0}
.d-sheet .card>.eyebrow{font-size:.6875rem;padding:10px 16px;border-bottom:1px solid var(--fg);background:var(--fg);color:var(--bg)}
.d-sheet .steps3{gap:0}
.d-sheet .steps3 li{grid-template-columns:1fr;gap:6px;padding:16px;border-bottom:1px solid var(--line)}
.d-sheet .mk{font-size:.625rem;color:var(--muted);display:flex;justify-content:space-between}
.d-sheet .steps3 li:nth-child(1) .mk::before{content:"Step 1"}
.d-sheet .steps3 li:nth-child(2) .mk::before{content:"Step 2"}
.d-sheet .steps3 li:nth-child(3) .mk::before{content:"Step 3"}
.d-sheet .steps3 b{font-weight:700;font-size:1.0625rem;font-stretch:106%}
.d-sheet .steps3 p{font-size:.875rem;line-height:1.5;color:var(--muted)}
.d-sheet .install-boards{font-size:.875rem}
.d-sheet .install-boards legend{font-size:.625rem;color:var(--muted)}
.d-sheet .install-boards input{accent-color:var(--fg)}
.d-sheet .install button{border-radius:0;padding:14px 16px;font-size:.75rem;box-shadow:inset 0 -3px 0 rgb(0 0 0/.18)}
.d-sheet .install-note{font-size:.8125rem;line-height:1.45}
.d-sheet .overview{margin:0;padding:12px 16px;border-top:0}
.d-sheet .overview summary{font-size:.875rem;font-weight:600}
.d-sheet .overview p{font-size:.8125rem}
@container (max-width:560px){.d-sheet .spec{grid-template-columns:1fr}.d-sheet .spec div+div{border-left:0;padding-left:0;border-top:1px solid var(--line)}.d-sheet .spec div{grid-template-columns:7em 1fr;align-items:baseline}}
"""

# B: lab notebook. Grid pad, mono headings, checklist steps, margin notes.
NOTE = """
.d-note{font:400 1.0625rem/1.6 "Atkinson Hyperlegible Next",system-ui,sans-serif;-webkit-font-smoothing:antialiased;
  background-image:linear-gradient(var(--grid) 1px,transparent 1px),linear-gradient(90deg,var(--grid) 1px,transparent 1px);background-size:24px 24px;background-position:-1px -1px}
.d-note h1,.d-note h2,.d-note .wordmark,.d-note .eyebrow,.d-note .tag,.d-note .mk,.d-note .beta,.d-note figcaption,.d-note .margin-note,.d-note .steps3 b,.d-note .install button,.d-note .install-boards legend,.d-note .fwshots figcaption b,.d-note .topnav .link{font-family:"Atkinson Hyperlegible Mono",ui-monospace,monospace}
.d-note .topnav{border-bottom:1px solid var(--fg)}
.d-note .topnav .link{font-size:.875rem}
.d-note .wordmark{font-weight:700;font-size:1.0625rem}
.d-note .wordmark i{width:10px;height:10px;border:2px solid var(--accent);background:transparent}
.d-note .beta{font-size:.75rem;color:var(--muted)}
.d-note .beta::before{content:"["}.d-note .beta::after{content:"]"}
.d-note h1{font-size:clamp(1.75rem,4cqi,2.75rem);line-height:1.12;letter-spacing:-.04em;font-weight:700;max-width:14em}
.d-note .sub{margin-top:12px;font-size:clamp(1.1875rem,2cqi,1.375rem);line-height:1.4;color:var(--muted)}
.d-note .rig-fig{background:var(--panel);border:1px solid var(--line);padding:16px 16px 12px;border-radius:4px}
.d-note .rig-fig figcaption{margin-top:8px;font-size:.8125rem;color:var(--muted)}
.d-note .feature{border-top:1px dashed var(--muted)}
.d-note .tag{font-size:.8125rem;color:var(--accent);margin-bottom:6px}
.d-note .tag::before{content:"> "}
.d-note .feature h2{font-size:1.375rem;line-height:1.25;letter-spacing:-.03em;font-weight:700}
.d-note .feature p{margin-top:10px;color:var(--muted)}
.d-note .margin-note{margin-top:16px;font-size:.875rem;color:var(--fg);border-left:2px solid var(--accent);padding:2px 0 2px 12px;max-width:34em}
.d-note .margin-note::before{content:"note: ";color:var(--accent);font-weight:700}
.d-note .fwshots img{border-radius:4px;border:1px solid var(--line)}
.d-note .fwshots figcaption{font-size:.8125rem;color:var(--muted);line-height:1.35}
.d-note .fwshots figcaption b{font-weight:700;font-size:.8125rem}
.d-note .fwmore{border:1px dashed var(--muted);border-radius:4px;background:var(--panel);font-size:.8125rem;color:var(--muted)}
.d-note .fwmore b{font-family:"Atkinson Hyperlegible Mono",ui-monospace,monospace;font-size:1.375rem;color:var(--fg)}
.d-note .card{background:var(--panel);border:1px solid var(--fg);border-radius:4px;padding:20px;box-shadow:4px 4px 0 var(--line)}
.d-note .eyebrow{font-size:.8125rem;font-weight:700;color:var(--fg)}
.d-note .eyebrow::before{content:"## ";color:var(--muted)}
.d-note .steps3 li{grid-template-columns:20px minmax(0,1fr);position:relative}
.d-note .steps3 li::before{content:"";width:16px;height:16px;margin-top:4px;border:1.5px solid var(--fg);border-radius:2px}
.d-note .mk{grid-column:2;font-size:.75rem;color:var(--accent);margin-bottom:-8px}
.d-note .steps3{counter-reset:s}.d-note .steps3 li{counter-increment:s}
.d-note .mk::before{content:"step " counter(s) " \\00b7  "}
.d-note .steps3 li>div{grid-column:2}
.d-note .steps3 b{font-weight:700;font-size:1rem;letter-spacing:-.02em}
.d-note .steps3 p{font-size:.9375rem;line-height:1.5;color:var(--muted)}
.d-note .install-boards{font-size:.9375rem}
.d-note .install-boards legend{font-size:.8125rem;font-weight:700}
.d-note .install button{border-radius:4px;padding:12px 16px;font-weight:700;font-size:.9375rem}
.d-note .install-note{font-size:.875rem;line-height:1.45}
.d-note .overview{border-top:1px dashed var(--muted)}
.d-note .overview summary{font-weight:700;font-size:.9375rem}
.d-note .overview p{font-size:.875rem}
"""

# C: current voice, instrumented. Hairlines, tighter radius, mono only for numbers and markers.
INST = """
.d-inst{font:400 1.0625rem/1.6 "Commissioner",system-ui,-apple-system,"Segoe UI",sans-serif;-webkit-font-smoothing:antialiased}
.d-inst .mono,.d-inst .readout,.d-inst .mk,.d-inst .beta,.d-inst .eyebrow,.d-inst .fwshots figcaption b,.d-inst .fwmore b{font-family:"Geist Mono",ui-monospace,monospace;font-variant-numeric:tabular-nums}
.d-inst .topnav{border-bottom:1px solid var(--line)}
.d-inst .topnav .link{font-size:.9375rem}
.d-inst .wordmark{font-weight:700;letter-spacing:-.015em;font-size:1.125rem}
.d-inst .wordmark i{width:10px;height:10px;border-radius:50%;background:var(--accent)}
.d-inst .beta{font-size:.6875rem;font-weight:500;letter-spacing:.04em;text-transform:uppercase;color:var(--muted);border:1px solid var(--line);border-radius:4px;padding:1px 6px}
.d-inst h1{font-size:clamp(1.875rem,4.2cqi,3.125rem);line-height:1.04;letter-spacing:-.03em;font-weight:660;max-width:14em}
.d-inst .sub{margin-top:12px;font-size:clamp(1.25rem,2.2cqi,1.375rem);line-height:1.35;color:var(--muted);font-weight:500;letter-spacing:-.01em}
.d-inst .readout{margin:28px 0 0;display:grid;grid-template-columns:repeat(4,minmax(0,1fr));border-block:1px solid var(--line);max-width:540px}
.d-inst .readout div{padding:10px 12px;display:grid;gap:2px;min-width:0}
.d-inst .readout div:first-child{padding-left:0}
.d-inst .readout div+div{border-left:1px solid var(--line)}
.d-inst .readout dt{font-size:.6875rem;letter-spacing:.04em;text-transform:uppercase;color:var(--muted)}
.d-inst .readout dd{margin:0;font-size:.8125rem;color:var(--muted)}
.d-inst .readout dd b{font-size:1.125rem;font-weight:500;color:var(--fg);letter-spacing:-.02em}
.d-inst .rig-fig{margin-top:24px}
.d-inst .feature{border-top:1px solid var(--line)}
.d-inst .feature h2{font-size:1.3125rem;line-height:1.25;letter-spacing:-.015em;font-weight:650}
.d-inst .feature p{margin-top:8px;color:var(--muted)}
.d-inst .fwshots img{border-radius:6px;box-shadow:0 0 0 1px var(--line)}
.d-inst .fwshots figcaption{font-size:.8125rem;color:var(--muted);line-height:1.35}
.d-inst .fwshots figcaption b{font-size:.75rem;font-weight:500;letter-spacing:.02em;margin-bottom:1px}
.d-inst .fwmore{border-radius:6px;box-shadow:inset 0 0 0 1px var(--line);font-size:.8125rem;line-height:1.3;color:var(--muted)}
.d-inst .fwmore b{font-size:1.25rem;font-weight:500;color:var(--fg)}
.d-inst .card{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:22px}
.d-inst .eyebrow{font-size:.6875rem;letter-spacing:.06em;text-transform:uppercase;color:var(--muted);font-weight:500}
.d-inst .steps3{gap:0}
.d-inst .steps3 li{grid-template-columns:34px minmax(0,1fr);padding-block:16px;border-top:1px solid var(--line)}
.d-inst .steps3 li:first-child{border-top:0;padding-top:4px}
.d-inst .mk{font-size:.6875rem;color:var(--accent);border:1px solid currentColor;border-radius:4px;height:20px;display:grid;place-items:center;margin-top:1px}
.d-inst .steps3 b{font-weight:620}
.d-inst .steps3 p{font-size:.9375rem;line-height:1.5;color:var(--muted)}
.d-inst .install-boards{font-size:.9375rem}
.d-inst .install-boards legend{font-weight:600;font-size:.875rem}
.d-inst .install button{border-radius:8px;padding:13px 22px;font-weight:600;line-height:1.2}
.d-inst .install-note{font-size:.875rem;line-height:1.45}
.d-inst .overview summary{font-weight:550;font-size:.9375rem}
.d-inst .overview p{font-size:.875rem;line-height:1.5}
@container (max-width:560px){.d-inst .readout{grid-template-columns:repeat(2,minmax(0,1fr))}.d-inst .readout div:nth-child(3){border-left:0;padding-left:0}.d-inst .readout div:nth-child(n+3){border-top:1px solid var(--line)}}
"""

SHELL = """
:root{--shell-bg:#f3f3f1;--shell-fg:#1a1b19;--shell-muted:#62655f;--shell-line:#d6d7d2;--shell-panel:#ffffff;--shell-ink:#1a1b19}
@media (prefers-color-scheme: dark){:root:not([data-theme="light"]){--shell-bg:#121311;--shell-fg:#e7e8e4;--shell-muted:#9a9d96;--shell-line:#2c2e2a;--shell-panel:#1a1b19;--shell-ink:#e7e8e4;color-scheme:dark}}
:root[data-theme="dark"]{--shell-bg:#121311;--shell-fg:#e7e8e4;--shell-muted:#9a9d96;--shell-line:#2c2e2a;--shell-panel:#1a1b19;--shell-ink:#e7e8e4;color-scheme:dark}
body{background:var(--shell-bg);color:var(--shell-fg);font:400 15px/1.55 system-ui,-apple-system,"Segoe UI",sans-serif}
.shell{max-width:1240px;margin:0 auto;padding-block:28px 64px;padding-left:max(16px,env(safe-area-inset-left,0px));padding-right:max(16px,env(safe-area-inset-right,0px));display:grid;gap:20px}
.shell h1{margin:0;font-size:1.375rem;letter-spacing:-.01em}
.shell .lede{margin:4px 0 0;color:var(--shell-muted);max-width:62ch}
.rec{border:1px solid var(--shell-line);background:var(--shell-panel);border-radius:10px;padding:14px 16px;display:grid;gap:4px;max-width:78ch}
.rec b{font-size:.75rem;letter-spacing:.06em;text-transform:uppercase;color:var(--shell-muted)}
.rec p{margin:0}
.bar{display:flex;flex-wrap:wrap;gap:10px 20px;align-items:center;position:sticky;top:env(safe-area-inset-top,0px);z-index:5;background:var(--shell-bg);padding-block:10px;border-bottom:1px solid var(--shell-line)}
body::before{content:"";position:fixed;inset:0 0 auto;height:env(safe-area-inset-top,0px);z-index:10;background:var(--shell-bg)}
.seg{display:inline-flex;border:1px solid var(--shell-line);border-radius:8px;overflow:hidden;flex-wrap:wrap}
.seg button{font:inherit;font-size:.875rem;background:transparent;color:var(--shell-fg);border:0;padding:7px 12px;cursor:pointer}
.seg button+button{border-left:1px solid var(--shell-line)}
.seg button[aria-pressed="true"],.seg button[aria-selected="true"]{background:var(--shell-ink);color:var(--shell-bg)}
.seg button:focus-visible{outline:2px solid var(--shell-ink);outline-offset:-4px}
.ctl{display:flex;gap:8px;align-items:center;color:var(--shell-muted);font-size:.8125rem}
.notes{display:grid;grid-template-columns:minmax(0,1.3fr) minmax(0,1fr);gap:24px;margin-bottom:20px}
.notes h2{margin:0 0 6px;font-size:1.125rem}
.notes p{margin:0;max-width:66ch}
.notes dl{margin:0;display:grid;gap:10px;font-size:.875rem}
.notes dt{font-size:.6875rem;letter-spacing:.06em;text-transform:uppercase;color:var(--shell-muted)}
.notes dd{margin:2px 0 0}
.sw{display:flex;flex-wrap:wrap;gap:4px;align-items:center}
.sw i{width:22px;height:22px;border-radius:4px;box-shadow:inset 0 0 0 1px rgb(127 127 127/.35)}
.sw span{font-size:.75rem;color:var(--shell-muted);width:3.2em}
.caveat{font-size:.8125rem;color:var(--shell-muted);margin-top:10px!important}
@media (max-width:760px){.notes{grid-template-columns:1fr}}
@media (prefers-reduced-motion:reduce){.frame{transition:none}}
"""

SCRIPT = """
(function(){
  var ids = %s, tabs = document.querySelectorAll('[role=tab]'), scheme = '', width = 'full';
  function show(id){
    if (ids.indexOf(id) < 0) id = ids[0];
    tabs.forEach(function(t){ t.setAttribute('aria-selected', t.dataset.id === id); t.tabIndex = t.dataset.id === id ? 0 : -1; });
    document.querySelectorAll('[role=tabpanel]').forEach(function(p){ p.hidden = p.id !== 'p-' + id; });
    try { localStorage.setItem('dir', id); } catch (e) {}
  }
  function apply(){
    document.querySelectorAll('.frame').forEach(function(f){
      if (scheme) f.setAttribute('data-scheme', scheme); else f.removeAttribute('data-scheme');
      f.setAttribute('data-width', width);
    });
  }
  tabs.forEach(function(t){
    t.addEventListener('click', function(){ show(t.dataset.id); history.replaceState(null, '', '#' + t.dataset.id); });
    t.addEventListener('keydown', function(e){
      var i = ids.indexOf(t.dataset.id), n = e.key === 'ArrowRight' ? i + 1 : e.key === 'ArrowLeft' ? i - 1 : -9;
      if (n === -9) return; n = (n + ids.length) %% ids.length; show(ids[n]); tabs[n].focus();
    });
  });
  document.querySelectorAll('[data-scheme-btn]').forEach(function(b){ b.addEventListener('click', function(){
    scheme = b.dataset.schemeBtn; document.querySelectorAll('[data-scheme-btn]').forEach(function(o){ o.setAttribute('aria-pressed', o === b); }); apply(); }); });
  document.querySelectorAll('[data-width-btn]').forEach(function(b){ b.addEventListener('click', function(){
    width = b.dataset.widthBtn; document.querySelectorAll('[data-width-btn]').forEach(function(o){ o.setAttribute('aria-pressed', o === b); }); apply(); }); });
  // Links inside the renders are stand-ins: keep them from changing the tab.
  document.querySelectorAll('.frame a[href^="#"]').forEach(function(a){ a.addEventListener('click', function(e){ e.preventDefault(); }); });
  var start = location.hash.slice(1);
  if (ids.indexOf(start) < 0) { try { start = localStorage.getItem('dir') || ''; } catch (e) {} }
  show(start); apply();
})();
"""


def swatches(vals):
    keys = ["bg", "panel", "fg", "muted", "line", "accent"]
    return "".join(f'<i style="background:{vals[k]}" title="--{k} {vals[k]}"></i>' for k in keys)


def main():
    css = SHELL + COMMON + BASE + SHEET + NOTE + INST + "".join(theme_css("." + d["cls"], d) for d in DIRS)
    tabs = "".join(f'<button type="button" role="tab" id="t-{d["id"]}" data-id="{d["id"]}" aria-controls="p-{d["id"]}">{d["tab"]}</button>'
                   for d in DIRS)
    panels = ""
    for d in DIRS:
        caveat = ("" if d["id"] == "current" else
                  '<p class="caveat">Copy is the live site\'s, unchanged. Mono labels and spec rows are a visual device '
                  'built from facts already on the page, not a copy proposal.</p>')
        panels += f"""
<section role="tabpanel" id="p-{d['id']}" aria-labelledby="t-{d['id']}" hidden>
  <div class="notes">
    <div><h2>{d['name']}</h2><p>{d['body']}</p>{caveat}</div>
    <dl>
      <div><dt>Fonts</dt><dd>{d['pair']}</dd></div>
      <div><dt>Type scale, px</dt><dd>{d['scale']}</dd></div>
      <div><dt>Palette</dt><dd><div class="sw"><span>Light</span>{swatches(d['light'])}</div><div class="sw" style="margin-top:4px"><span>Dark</span>{swatches(d['dark'])}</div></dd></div>
    </dl>
  </div>
  <div class="frame {d['cls']}" data-width="full"><div class="site">{page(d)}</div></div>
</section>"""
    rec = "".join(f"<p>{line}</p>" for line in RECOMMEND)
    html = f"""<title>Outrigger Visual Directions</title>
<meta name="claude:viewport-fit-sides" content="cover">
<link rel="preconnect" href="https://fonts.googleapis.com"><link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link rel="stylesheet" href="{FONTS}">
<style>{css}</style>
{sprite()}
<div class="shell">
  <header>
    <h1>Outrigger homepage: three directions</h1>
    <p class="lede">The live hero, &ldquo;Set up once&rdquo; and the firmware feature, rendered in today&rsquo;s look and three alternatives.
    Same copy everywhere. Switch the theme and width to see each one in dark mode and at phone width.</p>
  </header>
  <div class="rec"><b>Recommendation</b>{rec}</div>
  <div class="bar">
    <div class="seg" role="tablist" aria-label="Direction">{tabs}</div>
    <div class="ctl">Theme <span class="seg"><button type="button" data-scheme-btn="" aria-pressed="true">Auto</button><button type="button" data-scheme-btn="light" aria-pressed="false">Light</button><button type="button" data-scheme-btn="dark" aria-pressed="false">Dark</button></span></div>
    <div class="ctl">Width <span class="seg"><button type="button" data-width-btn="full" aria-pressed="true">Full</button><button type="button" data-width-btn="phone" aria-pressed="false">Phone</button></span></div>
  </div>
  {panels}
</div>
<script>{SCRIPT % repr([d['id'] for d in DIRS]).replace("'", '"')}</script>
"""
    OUT.write_text(html.encode("ascii", "xmlcharrefreplace").decode())  # entities, so it reads right without a charset meta
    print(f"wrote {OUT.relative_to(HERE.parent.parent.parent)} ({len(html) // 1024} KB)")


if __name__ == "__main__":
    main()
