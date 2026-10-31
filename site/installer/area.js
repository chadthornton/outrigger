// The install area under each tab on site/index.html: its buttons, and the
// four states it moves through in place (ready, board read, installing,
// done). No dialog: the only pop-up is the browser's own port picker.
//
//   <div class="install" data-builds='[{"label":"Install","manifest":"firmware/.../hub/manifest.json"}]'></div>
//
// Without JavaScript, or in a browser without Web Serial, the area says so and
// the text around it still reads on its own.
import { ESPLoader } from "../vendor/esptool-js-0.7.0/bundle.js";
import { ChunkedTransport, readFlashInPieces } from "./serial.js";
import { describeBoard, buildName } from "./board.js";
import { readBoard, fetchBuild, install, backupStore, finish } from "./install.js";

// Shown under each install area and in Details, so a bug report says which
// installer ran. Bump it with every change to site/installer/.
export const INSTALLER_VERSION = "2026-10-31.7";

// The ESP32-S3's own USB port (USB-Serial-JTAG), in Computer mode or BOOT+RST.
const PORT_FILTER = [{ usbVendorId: 0x303a, usbProductId: 0x1001 }];
// The board's own USB port runs at full USB speed whatever baud rate is set,
// and asking its ROM to change baud hangs the connection, so stay at 115200.
const BAUD = 115200;
// Web Serial's default 255-byte buffer can't take a flash-read packet (up to
// 4 KiB, more once SLIP-escaped): reads stalled after "Flash ID" in Brave.
// ESP Web Tools opened this board with 8192.
const SERIAL_OPTIONS = { bufferSize: 16384 };
const READ_TIMEOUT_MS = 30000;
// esptool-js's progress lines: the latest shows while the board is read, and
// the last few show under "Details" when something fails.
const LOG = [];
let onLogLine = () => {};
const logLine = (line) => {
  const text = String(line).trim();
  if (!text) return;
  LOG.push(`${(performance.now() / 1000).toFixed(1)} s  ${text}`);
  if (LOG.length > 60) LOG.shift();
  console.debug("esptool:", text);
  onLogLine(text);
};
const TERMINAL = { clean() {}, writeLine: logLine, write: logLine };
async function withTimeout(promise, ms, what) {
  let timer;
  try {
    return await Promise.race([promise,
      new Promise((_, reject) => { timer = setTimeout(() => reject(new Error(`${what} took longer than ${ms / 1000} s.`)), ms); })]);
  } finally {
    clearTimeout(timer);
  }
}
// One install at a time on the page: every area shares the one board and the log.
let busy = false;

const KEEPS = "Installing replaces the program only. Settings, Wi-Fi and FM-1 backups stay.";

const h = (tag, attrs = {}, ...kids) => {
  const el = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === "on") Object.entries(v).forEach(([ev, fn]) => el.addEventListener(ev, fn));
    else if (v !== false && v != null) el.setAttribute(k, v === true ? "" : v);
  }
  el.append(...kids.flat().filter((k) => k != null && k !== false));
  return el;
};
const mb = (n) => (n / 1048576).toFixed(1);

function saveFile(bytes, name) {
  const url = URL.createObjectURL(new Blob([bytes], { type: "application/octet-stream" }));
  const a = h("a", { href: url, download: name });
  document.body.append(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 60000);
}

class InstallArea {
  constructor(el) {
    this.el = el;
    this.builds = JSON.parse(el.dataset.builds);
    this.helpHref = el.dataset.help || "#no-port";
    this.trace = "";
    this.ready();
  }

  show(...kids) {
    this.el.replaceChildren(...kids.filter((k) => k != null));
  }

  ready(note) {
    if (!("serial" in navigator)) {
      return this.show(h("p", { class: "install-note" }, window.isSecureContext
        ? "This browser can't install. Use Chrome or Edge on a computer."
        : "Installing needs a secure page: open this over https:// or from localhost."));
    }
    this.show(
      note ? h("p", { class: "install-error", role: "alert" }, note) : null,
      note && LOG.length ? h("details", { class: "install-log" }, h("summary", {}, "Details"),
        h("pre", {}, [`Installer ${INSTALLER_VERSION}`, ...LOG.slice(-25), "", ...this.trace.split("\n").slice(-40).map((l) => l.slice(0, 160))].join("\n"))) : null,
      h("div", { class: "row" }, this.builds.map((b, i) =>
        h("button", { class: i ? "alt" : null, on: { click: () => this.connect(b) } }, b.label))),
      h("p", { class: "install-note" }, KEEPS, " ", h("a", { href: this.helpHref }, "No board listed?")),
      h("p", { class: "install-version" }, `Installer ${INSTALLER_VERSION}`),
    );
  }

  async connect(choice) {
    if (busy) return;
    let port;
    try {
      port = await navigator.serial.requestPort({ filters: PORT_FILTER });
    } catch {
      return; // closed the picker
    }
    busy = true;
    try {
      this.show(h("p", { class: "install-note", role: "status" }, "Downloading the firmware…"));
      let build;
      try {
        build = await fetchBuild(choice.manifest);
      } catch (e) {
        return this.ready(this.explain(e, "Couldn't download the firmware."));
      }
      const step = h("span", {});
      this.show(h("p", { class: "install-note", role: "status" }, "Reading the board… ", step));
      onLogLine = (text) => { if (!text.startsWith("Debug")) step.textContent = text; };
      try {
        // Only the board read is timed: a slow download isn't a cable problem.
        const reading = this.open(port).then((loader) => readBoard(loader));
        reading.catch(() => {}); // after a timeout it fails against the closed port; nothing waits for it
        const board = await withTimeout(reading, READ_TIMEOUT_MS, "Reading the board");
        this.confirm(choice, build, board);
      } catch (e) {
        await this.close();
        this.ready(this.explain(e, "Couldn't read the board."));
      }
    } finally {
      busy = false;
      onLogLine = () => {};
    }
  }

  async open(port) {
    this.transport = new ChunkedTransport(port, true); // trace: kept for Details when something fails
    this.loader = new ESPLoader({ transport: this.transport, baudrate: BAUD, romBaudrate: BAUD, serialOptions: SERIAL_OPTIONS, terminal: TERMINAL });
    const loader = this.loader;
    loader.readFlash = (addr, size, onPacket) => readFlashInPieces(loader, addr, size, onPacket || undefined);
    const chip = await loader.main();
    if (!/ESP32-S3/.test(chip)) throw new Error(`That's an ${chip}, not the Waveshare's ESP32-S3.`);
    return loader;
  }

  async close() {
    if (this.transport) {
      this.trace = this.transport.traceLog || "";
      try {
        await Promise.race([this.transport.disconnect(), new Promise((r) => setTimeout(r, 2000))]);
      } catch {
        // already gone
      }
    }
    this.transport = this.loader = null;
  }

  confirm(choice, build, board) {
    const words = describeBoard(board, build.manifest.build);
    const save = board.store
      ? h("label", { class: "check" }, h("input", { type: "checkbox" }),
        ` Save a copy of my backups to this computer first (${mb(board.storeRange.size)} MB, about ${Math.ceil(board.storeRange.size / 1048576 * 41 / 60)} min)`)
      : null;
    const name = buildName(build.manifest.build.app_set);
    this.show(
      h("p", { class: "install-read" }, words.board),
      h("p", {}, words.install),
      save,
      h("div", { class: "row" },
        h("button", { on: { click: () => this.run(build, board, save?.querySelector("input").checked) } }, `Install ${name}`),
        h("button", { class: "alt", on: { click: async () => { await this.close(); this.ready(); } } }, "Cancel")),
    );
  }

  progress(label, done, total) {
    const bar = h("progress", { max: total || 1, value: done || 0 });
    this.show(bar, h("p", { class: "install-note", role: "status" }, label, ". Keep the cable in."));
  }

  async run(build, board, saveFirst) {
    if (busy) return;
    busy = true;
    try {
      if (saveFirst) {
        const t0 = performance.now();
        const image = await backupStore(this.loader, board.storeRange, (done, total) =>
          this.progress(`Saving your backups: ${mb(done)} of ${mb(total)} MB`, done, total));
        const tag = board.tag ? board.tag.commit : "earlier";
        saveFile(image, `fm1-hub-backups-${tag}-${new Date().toISOString().slice(0, 10)}.bin`);
        console.info(`store read: ${image.length} bytes in ${((performance.now() - t0) / 1000).toFixed(1)} s`);
      }
      await install({
        loader: this.loader, board, manifest: build.manifest, parts: build.parts,
        onStep: (s) => this.progress(s.step === "verify" ? "Checking every part" : `Writing the ${s.part}`, s.written, s.total),
      });
      // Verified: from here on it's installed, whatever the restart does.
      const { loader, transport } = this;
      this.transport = this.loader = null;
      await finish({ loader, transport });
      this.done(build);
    } catch (e) {
      await this.close();
      this.ready(this.explain(e, "The install stopped."));
    } finally {
      busy = false;
    }
  }

  done(build) {
    const name = `${buildName(build.manifest.build.app_set)} ${build.manifest.build.commit}`;
    this.show(h("div", { class: "install-done", role: "status" },
      h("p", {}, h("b", {}, `Installed ${name}.`), " The board restarts by itself. If its screen stays dark, tap RST once."),
      h("p", {}, "Then unplug it from this computer and plug it back into the hub.")));
  }

  explain(e, lead) {
    const msg = String(e && e.message ? e.message : e);
    if (/Nothing was written|didn't read back|download|not the Waveshare/.test(msg)) return `${lead} ${msg}`;
    return `${lead} Check the cable is a data cable and the board is in Computer mode (or BOOT+RST), then try again. (${msg})`;
  }
}

for (const el of document.querySelectorAll(".install[data-builds]")) new InstallArea(el);
