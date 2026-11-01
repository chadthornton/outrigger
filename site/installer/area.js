// The install area under each tab on site/index.html: its buttons, and the
// four states it moves through in place (ready, board read, installing,
// done). No dialog: the only pop-up is the browser's own port picker.
//
//   <div class="install" data-builds='[{"label":"Install","manifest":"firmware/.../hub/manifest.json",
//     "board":"Waveshare 1.69\"","tag_id":0}, ...]'></div>
//
// One entry per button and board (firmware/boards.json). With more than one
// board the area asks which one first: the chip can't tell which board it sits
// on, and the wrong build leaves the screen dark.
//
// Without JavaScript, or in a browser without Web Serial, the area says so and
// the text around it still reads on its own.
import { ESPLoader } from "../vendor/esptool-js-0.7.0/bundle.js";
import { ChunkedTransport, readFlashInPieces } from "./serial.js";
import { describeBoard, buildName } from "./board.js";
import { readBoard, fetchBuild, install, finish, prepareStore } from "./install.js";

// Shown under each install area and in Details, so a bug report says which
// installer ran. Bump it with every change to site/installer/.
export const INSTALLER_VERSION = "2026-11-01.2";

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
const STORE_TIMEOUT_MS = 120000;
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

// Chad's wording (2026-10-31) for a store region that holds data but no store.
const UNREADABLE = "Your ESP32 has storage we can't read. Email feedback@outrigger.music before continuing. Nothing was installed.";

const KEEPS = "Checking reads the board and changes nothing: nothing is installed until you confirm. Installing replaces the program only. Settings, Wi-Fi and FM-1 backups stay.";

const h = (tag, attrs = {}, ...kids) => {
  const el = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === "on") Object.entries(v).forEach(([ev, fn]) => el.addEventListener(ev, fn));
    else if (v !== false && v != null) el.setAttribute(k, v === true ? "" : v);
  }
  el.append(...kids.flat().filter((k) => k != null && k !== false));
  return el;
};

let areas = 0;

class InstallArea {
  constructor(el) {
    this.el = el;
    this.builds = JSON.parse(el.dataset.builds);
    this.boards = [...new Map(this.builds.filter((b) => b.board).map((b) => [b.board, b])).values()];
    this.names = Object.fromEntries(this.boards.map((b) => [b.tag_id, b.board]));
    this.board = this.boards.length === 1 ? this.boards[0].board : null;
    this.id = ++areas;
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
      this.boards.length > 1 ? this.boardPicker() : null,
      h("div", { class: "row" }, [...new Set(this.builds.map((b) => b.label))].map((label, i) =>
        h("button", { class: i ? "alt" : null, "data-needs-board": this.boards.length > 1, disabled: !this.board && this.boards.length > 1,
          on: { click: () => this.connect(this.builds.find((b) => b.label === label && (!b.board || b.board === this.board))) } }, label))),
      h("p", { class: "install-note" }, KEEPS, " ", h("a", { href: this.helpHref }, "No board listed?")),
      h("p", { class: "install-version" }, `Installer ${INSTALLER_VERSION}`),
    );
  }

  boardPicker() {
    const pick = (board) => {
      this.board = board;
      for (const b of this.el.querySelectorAll("button[data-needs-board]")) b.disabled = false;
    };
    return h("fieldset", { class: "install-boards" }, h("legend", {}, "Which ESP32 do you have?"),
      this.boards.map((b) => h("label", {},
        h("input", { type: "radio", name: `board-${this.id}`, value: b.board, checked: this.board === b.board,
          on: { change: () => pick(b.board) } }), " ", b.board)));
  }

  async connect(choice) {
    if (busy || !choice) return;
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
        if (build.template && !board.store) this.show(h("p", { class: "install-note", role: "status" }, "Checking the board's store…"));
        // The region's MD5 takes seconds and each download has its own deadline; this bounds them all.
        const store = await withTimeout(prepareStore({ loader: this.loader, board, build }), STORE_TIMEOUT_MS, "Checking the board's store");
        if (store.action === "unreadable") {
          await this.close();
          return this.ready(UNREADABLE);
        }
        this.confirm(choice, build, board, store);
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
    if (!/ESP32-S3/.test(chip)) throw new Error(`That's an ${chip}, not an ESP32-S3.`);
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

  confirm(choice, build, board, store) {
    const words = describeBoard(board, build.manifest.build, this.names);
    const name = buildName(build.manifest.build.app_set);
    const noStore = store.unfinished ? "Its store was being set up when the last install stopped, so it has no store yet."
      : store.previousFirmware ? "This board ran other firmware before, so it has no store." : "It has no store yet.";
    if (store.downloadProblem) {
      // A store set up later would land on leftovers (store.js storeAction), so nothing installs without it.
      return this.show(
        h("p", { class: "install-read" }, words.board),
        words.warning ? h("p", { class: "install-error", role: "alert" }, words.warning) : null,
        h("p", {}, `${noStore} Installing sets one up with ${listOf(store.starter)}, but a download didn't come through: ${store.downloadProblem}. Nothing was written. Try again in a few minutes.`),
        h("div", { class: "row" }, h("button", { class: "alt", on: { click: async () => { await this.close(); this.ready(); } } }, "Close")),
      );
    }
    const storeWords = store.action !== "create" ? null
      : `${noStore} Installing also sets up a fresh one with ${store.starter.length} FM-1 firmwares to switch between: ${listOf(store.starter)}. ` +
        "They download from thefirmwarehub.com, and the file the loader needs from the FM-1's chip maker from GitHub, straight into this browser.";
    this.show(
      h("p", { class: "install-read" }, words.board),
      words.warning ? h("p", { class: "install-error", role: "alert" }, words.warning) : null,
      h("p", {}, words.install),
      storeWords ? h("p", {}, storeWords) : null,
      h("div", { class: "row" },
        h("button", { on: { click: () => this.run(build, board, store) } }, `Install ${name}`),
        h("button", { class: "alt", on: { click: async () => { await this.close(); this.ready(); } } }, "Cancel")),
    );
  }

  progress(label, done, total) {
    const bar = h("progress", { max: total || 1, value: done || 0 });
    this.show(bar, h("p", { class: "install-note", role: "status" }, label, ". Keep the cable in."));
  }

  async run(build, board, store) {
    if (busy) return;
    busy = true;
    const label = { verify: "Checking every part", "verify store": "Checking the new store" };
    try {
      const { storeProblem } = await install({
        loader: this.loader, board, manifest: build.manifest, parts: build.parts,
        storeWrites: store.storeWrites, storeWritable: store.storeWritable,
        onStep: (s) => this.progress(label[s.step] || `Writing the ${s.part}`, s.written, s.total),
      });
      // Verified: from here on it's installed, whatever the restart does.
      const { loader, transport } = this;
      this.transport = this.loader = null;
      await finish({ loader, transport });
      this.done(build, store, storeProblem);
    } catch (e) {
      await this.close();
      this.ready(this.explain(e, "The install stopped."));
    } finally {
      busy = false;
    }
  }

  done(build, store, storeProblem) {
    const name = `${buildName(build.manifest.build.app_set)} ${build.manifest.build.commit}`;
    const storeWords = store.action !== "create" ? null
      : storeProblem ? `Firmware switching isn't set up: ${storeProblem}.`
        : `Its store is set up with ${listOf(store.starter)}. Switch between them from the loader on the board.`;
    this.show(h("div", { class: "install-done", role: "status" },
      h("p", {}, h("b", {}, `Installed ${name}.`), " The board restarts by itself. If its screen stays dark, tap RST once."),
      storeWords ? h("p", {}, storeWords) : null,
      h("p", {}, "Then unplug it from this computer and plug it back into the hub.")));
  }

  explain(e, lead) {
    const msg = String(e && e.message ? e.message : e);
    if (/Nothing was written|didn't read back|download|not an ESP32-S3|template/.test(msg)) return `${lead} ${msg}`;
    return `${lead} Check the cable is a data cable and the board is in Computer mode (or BOOT+RST), then try again. (${msg})`;
  }
}

// "A, B and C"
function listOf(names) {
  return names.length < 2 ? names.join("") : `${names.slice(0, -1).join(", ")} and ${names.at(-1)}`;
}

for (const el of document.querySelectorAll(".install[data-builds]")) new InstallArea(el);
