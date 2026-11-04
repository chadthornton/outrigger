// The setup diagram, made explorable: pick a part (on the drawing or its chip) and the
// drawing zooms to it while its panel opens. Without JavaScript the static drawing and
// every panel show in order, so nothing is lost.
const VIEW = [0, 0, 640, 312];
const PARTS = {
  controller: [0, 185, 160, 66],
  outrigger: [276, 8, 88, 124],
  hub: [262, 176, 116, 48],
  fm1: [468, 196, 154, 92],
};

function zoomBox([x, y, w, h]) {
  // Pad the part and keep the drawing's aspect ratio, clamped to the full view.
  const aspect = VIEW[2] / VIEW[3];
  let bw = Math.max(w * 2.1, 440), bh = h * 2.1;  // a minimum, so a small part like the hub keeps its neighbours in view
  if (bw / bh > aspect) bh = bw / aspect; else bw = bh * aspect;
  const bx = Math.min(Math.max(x + w / 2 - bw / 2, 0), VIEW[2] - bw);
  const by = Math.min(Math.max(y + h / 2 - bh / 2, 0), VIEW[3] - bh);
  return [bx, by, bw, bh];
}

function animate(svg, to, ms = 420) {
  const from = svg.getAttribute('viewBox').split(' ').map(Number);
  if (matchMedia('(prefers-reduced-motion: reduce)').matches) ms = 0;
  const start = performance.now();
  const ease = (t) => 1 - Math.pow(1 - t, 3);
  function step(now) {
    const t = ms ? Math.min((now - start) / ms, 1) : 1;
    svg.setAttribute('viewBox', from.map((f, i) => f + (to[i] - f) * ease(t)).join(' '));
    if (t < 1) requestAnimationFrame(step);
  }
  requestAnimationFrame(step);
}

const DRAWING = `
  <g class="wires" fill="none" stroke="var(--accent)" stroke-width="4" stroke-linecap="round">
    <path d="M150 230 C 210 230, 230 200, 268 200"/><path d="M372 200 C 410 200, 430 240, 470 240"/><path d="M320 180 L 320 128"/>
  </g>
  <g class="part" data-part="controller"><rect class="hit" x="-22" y="163" width="204" height="110" rx="16"/><use href="art.svg#keyboard" x="0" y="185" width="160" height="66"/></g>
  <g class="part" data-part="hub"><rect class="hit" x="240" y="154" width="160" height="92" rx="16"/><use href="art.svg#hub" x="262" y="176" width="116" height="48"/></g>
  <g class="part" data-part="fm1"><rect class="hit" x="446" y="174" width="194" height="136" rx="16"/><use href="art.svg#fm1" x="468" y="196" width="154" height="92"/></g>
  <g class="part" data-part="outrigger"><rect class="hit" x="254" y="-14" width="132" height="168" rx="16"/><use href="art.svg#board" x="276" y="8" width="88" height="124"/></g>
  <g class="labels" style="font-family: var(--sans)" font-size="16" font-weight="600" fill="currentColor">
    <text x="80" y="282" text-anchor="middle">MIDI controller</text><text x="543" y="306" text-anchor="middle">FM-1</text>
    <text x="376" y="74">Outrigger</text><text x="376" y="93" font-size="14" font-weight="400" opacity=".7">a little ESP32 device</text><text x="320" y="250" text-anchor="middle" font-weight="400" opacity=".7">Powered USB hub</text>
  </g>
  <g class="dots">
<circle class="ring" cx="150" cy="190" r="11"/><circle cx="150" cy="190" r="11"/><circle class="ring" cx="362" cy="22" r="11"/><circle cx="362" cy="22" r="11"/><circle class="ring" cx="372" cy="182" r="11"/><circle cx="372" cy="182" r="11"/><circle class="ring" cx="612" cy="198" r="11"/><circle cx="612" cy="198" r="11"/>
  </g>`;

for (const fig of document.querySelectorAll('[data-rig]')) {
  const svg = fig.querySelector('svg');
  svg.innerHTML = DRAWING;
  svg.setAttribute('viewBox', VIEW.join(' '));
  fig.classList.add('live');
  const chips = [...fig.querySelectorAll('[data-show]')];
  const panels = [...fig.querySelectorAll('[data-panel]')];
  let current = null;

  function show(part) {
    current = part === current ? null : part;
    fig.dataset.zoomed = current || '';
    animate(svg, current ? zoomBox(PARTS[current]) : VIEW);
    for (const g of svg.querySelectorAll('.part')) g.classList.toggle('dim', !!current && g.dataset.part !== current);
    for (const c of chips) c.setAttribute('aria-pressed', c.dataset.show === current);
    for (const p of panels) p.hidden = p.dataset.panel !== current;
  }

  for (const g of svg.querySelectorAll('.part')) {
    g.setAttribute('tabindex', '0');
    g.setAttribute('role', 'button');
    g.addEventListener('click', () => show(g.dataset.part));
    g.addEventListener('keydown', (e) => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); show(g.dataset.part); } });
  }
  chips.forEach((c) => c.addEventListener('click', () => show(c.dataset.show)));
  fig.querySelectorAll('[data-reset]').forEach((b) => b.addEventListener('click', () => show(current)));
  fig.addEventListener('keydown', (e) => { if (e.key === 'Escape' && current) show(current); });
  panels.forEach((p) => (p.hidden = true));
}

// Install: check the browser when someone clicks, not before.
for (const btn of document.querySelectorAll('[data-install]')) {
  btn.addEventListener('click', () => {
    const out = document.getElementById(btn.dataset.install);
    const ok = 'serial' in navigator && isSecureContext;
    out.hidden = false;
    out.textContent = ok
      ? 'Mock-up: here the installer opens, in this page, and asks Chrome for your ESP32’s port.'
      : 'This browser can’t talk to the board. Open this page in Chrome or Edge on a Mac or PC.';
    out.classList.toggle('bad', !ok);
  });
}
