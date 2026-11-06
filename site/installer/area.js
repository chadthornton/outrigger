// The install area under each tab on site/index.html: its buttons, and the
// four states it moves through in place (ready, board read, installing,
// done). The one dialog of its own is "Report this board" (report_form.js),
// offered when the chip check finds no profile or refuses the chip.
//
//   <div class="install" data-builds='[{"label":"Install","manifest":"firmware/.../hub/manifest.json",
//     "board":"Waveshare 1.69\"","tag_id":0}, ...]'></div>
//
// One entry per button and board (firmware/boards.json), each with the
// board's flash_mb, psram_mb and tested; plus one with no manifest for
// "another board", which only checks the chip. With more than one board the
// area asks which one first: the chip can't tell which board it sits on, and
// the wrong build leaves the screen dark.
//
// Without JavaScript, or in a browser without Web Serial, the area says so and
// the text around it still reads on its own.
import { ESPLoader } from "../vendor/esptool-js-0.7.0/bundle.js";
import { ChunkedTransport, readFlashInPieces } from "./serial.js";
import { describeBoard, buildName } from "./board.js";
import { checkChip, USB_BRIDGES } from "./chip.js";
import { openReportForm, openRequestChoice, openLogForm } from "./report_form.js";
import { readBoard, fetchBuild, install, finish, prepareStore } from "./install.js";
import { buildInstallEvent, sendInstallEvent, countingOn, setCounting } from "./install_event.js";
import { PORT_FILTER, SERIAL_OPTIONS, port as lock, h } from "./ui.js";

// Shown in the page footer and in Details, so a bug report says which
// installer ran. Bump it with every change to site/installer/.
export const INSTALLER_VERSION = "2026-11-05.3";

// The page footer shows it (build.py), so a bug report says which installer ran.
const versionLine = typeof document !== "undefined" && document.getElementById("installer-version");
if (versionLine) { versionLine.textContent = `Installer ${INSTALLER_VERSION}`; versionLine.hidden = false; }

// Installing lists converter chips beside the ESP32-S3's own port (ui.js), so
// a board that can't host (a Cheap Yellow Display, say) shows up and hears why
// (chip.js) instead of not being listed.
const INSTALL_PORT_FILTER = [...PORT_FILTER, ...Object.keys(USB_BRIDGES).map((vid) => ({ usbVendorId: Number(vid) }))];
// The board's own USB port runs at full USB speed whatever baud rate is set,
// and asking its ROM to change baud hangs the connection, so stay at 115200.
const BAUD = 115200;
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

// Chad's wording (2026-10-31) for a store region that holds data but no store.
const UNREADABLE = "Your ESP32 has storage we can't read. Email feedback@outrigger.music before continuing. Nothing was installed.";

// The board restarts after an install; a board that stays dark needs RST. Its USB port hosts the hub once it runs,
// so this computer can't see it come back: the hint shows after the restart should have finished.
const RST_HINT_MS = 20000;

// The anonymous install count (install_event.js). Reading localStorage itself can throw; then nothing is remembered.
function storage() {
  try {
    return window.localStorage;
  } catch {
    return null;
  }
}


let areas = 0;

class InstallArea {
  constructor(el) {
    this.el = el;
    this.builds = JSON.parse(el.dataset.builds);
    this.boards = [...new Map(this.builds.filter((b) => b.board).map((b) => [b.board, b])).values()];
    this.names = Object.fromEntries(this.boards.filter((b) => b.manifest).map((b) => [b.tag_id, b.board]));
    this.profiles = this.boards.filter((b) => b.manifest);
    // Every board with a profile, built or not, for the chip check's suggestion.
    this.suggest = el.dataset.suggest ? JSON.parse(el.dataset.suggest) : this.profiles.map((p) => p.board);
    this.board = this.boards.length === 1 ? this.boards[0].board : null;
    this.id = ++areas;
    this.helpHref = el.dataset.help || "#no-port";
    this.trace = "";
    this.refused = null; // what the chip check read when it refused, for "Report this board"
    this.ready();
  }

  show(...kids) {
    this.el.replaceChildren(...kids.filter((k) => k != null));
  }

  // expanded: the board question is open. A refusal or failure opens it, so the visitor can try again.
  ready(note, expanded = !!note) {
    if (!expanded) this.board = this.boards.length === 1 ? this.boards[0].board : null;
    if (!("serial" in navigator)) {
      return this.show(h("p", { class: "install-note" }, window.isSecureContext
        ? "This browser can't install. Use Chrome or Edge on a computer."
        : "Installing needs a secure page: open this over https:// or from localhost."));
    }
    this.show(
      note ? h("p", { class: "install-error", role: "alert" }, note) : null,
      note && this.refused ? h("div", { class: "row" }, h("button", { class: "alt",
        on: { click: () => openReportForm({ ...this.refused, installer: INSTALLER_VERSION }) } }, "Ask us to add this board")) : null,
      // Opt-in, one click: the visitor sees what goes first (report_form.js openLogForm). Never the trace.
      note && LOG.length && !this.refused ? h("div", { class: "row" }, h("button", { class: "alt",
        on: { click: () => openLogForm({ log: LOG.slice(-40), error: note,
          picked: this.board || "Not asked", installer: INSTALLER_VERSION }) } }, "Send this log to Outrigger")) : null,
      note && LOG.length ? h("details", { class: "install-log" }, h("summary", {}, "Details"),
        h("pre", {}, [`Installer ${INSTALLER_VERSION}`, ...LOG.slice(-25), "", ...this.trace.split("\n").slice(-40).map((l) => l.slice(0, 160))].join("\n"))) : null,
      expanded && this.boards.length > 1 ? this.boardPicker() : null,
      h("div", { class: "row" }, this.mainButton(expanded)),
      this.links(),
    );
  }

  // One primary button. With several boards it first opens the board question ("Check my ESP32"), then
  // waits for an answer ("Continue", which starts the port chooser).
  mainButton(expanded) {
    const many = this.boards.length > 1;
    if (many && expanded) {
      this.continueButton = h("button", { disabled: !this.board,
        on: { click: () => this.connect(this.builds.find((b) => b.board === this.board)) } }, "Continue");
      return this.continueButton;
    }
    return h("button", { on: { click: () => many ? this.ready(undefined, true) : this.connect(this.builds[0]) } }, this.builds[0].label);
  }

  // Ticked by default; unticking it sends nothing and is remembered in this browser. Shown only beside the Install
  // button (confirm), and nothing is counted before that click, so no one is counted before they've seen it.
  countBox() {
    return h("label", { class: "install-count" },
      h("input", { type: "checkbox", checked: countingOn(storage()), on: { change: (e) => setCounting(storage(), e.target.checked) } }),
      " Send an anonymous install count (board and result, nothing else)");
  }

  // Fire and forget, only for a board with a build: the count must never hold up or break an install.
  // failure: "write" when the install stopped, or none when it finished (run(), after the Install click).
  count(choice, failure = null) {
    if (!choice || !choice.profile || !choice.edition) return;
    const event = buildInstallEvent({ board: choice.profile, edition: choice.edition, installer: INSTALLER_VERSION, failure });
    sendInstallEvent(event, { fetch: (...args) => fetch(...args), storage: storage() }).catch(() => {});
  }

  links() {
    const help = (e) => {
      e.preventDefault();
      const d = document.querySelector(this.helpHref);
      if (d) { d.open = true; d.scrollIntoView({ behavior: "smooth", block: "center" }); }
    };
    const request = (e) => {
      e.preventDefault();
      const read = this.builds.find((b) => !b.manifest);
      openRequestChoice({ installer: INSTALLER_VERSION, onRead: read ? () => { this.requested = true; this.connect(read); } : null });
    };
    return h("p", { class: "install-links" },
      h("a", { href: this.helpHref, on: { click: help } }, "I plugged it in, nothing's happening"),
      h("a", { href: "#", on: { click: request } }, "Request support for my ESP32 board"));
  }

  boardPicker() {
    // The chip can't say which board it sits on (chip.js), so the visitor does.
    const pick = (board) => {
      this.board = board;
      if (this.continueButton) this.continueButton.disabled = false;
    };
    return h("fieldset", { class: "install-boards" }, h("legend", {}, "Which one is it?"),
      this.boards.map((b) => h("label", {},
        h("input", { type: "radio", name: `board-${this.id}`, value: b.board, checked: this.board === b.board,
          on: { click: () => pick(b.board) } }), " ", b.board)));
  }

  async connect(choice) {
    if (lock.busy || !choice) return;
    this.refused = null;
    let port;
    try {
      port = await navigator.serial.requestPort({ filters: INSTALL_PORT_FILTER });
    } catch {
      return; // closed the picker
    }
    lock.busy = true;
    try {
      if (!choice.manifest) return await this.checkOnly(port);
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
        const reading = this.open(port, choice).then((loader) => readBoard(loader));
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
      lock.busy = false;
      onLogLine = () => {};
    }
  }

  // "Another board": only the chip check, then what it found.
  async checkOnly(port) {
    this.show(h("p", { class: "install-note", role: "status" }, "Reading the board…"));
    try {
      await withTimeout(this.open(port, null), READ_TIMEOUT_MS, "Reading the board");
    } catch (e) {
      await this.close();
      const asked = this.requested;
      this.requested = false;
      this.ready(this.explain(e, "Couldn't read the board."));
      // "Request support" said "plug it in and we'll read it": the refusal is the answer, so the form opens with it.
      if (asked && this.refused) openReportForm({ ...this.refused, installer: INSTALLER_VERSION });
      return;
    }
  }

  // Connects, then checks the chip before anything is read or written. picked:
  // the build's board entry, or null for "another board" (always refused).
  async open(port, picked) {
    this.transport = new ChunkedTransport(port, true); // trace: kept for Details when something fails
    this.loader = new ESPLoader({ transport: this.transport, baudrate: BAUD, romBaudrate: BAUD, serialOptions: SERIAL_OPTIONS, terminal: TERMINAL });
    const loader = this.loader;
    loader.readFlash = (addr, size, onPacket) => readFlashInPieces(loader, addr, size, onPacket || undefined);
    this.chipWords = null;
    const chip = await loader.main();
    const facts = {
      chip,
      features: await loader.chip.getChipFeatures(loader),
      flashId: await loader.readFlashId(),
      usbVendorId: port.getInfo().usbVendorId,
    };
    const verdict = checkChip(facts, picked, this.profiles, this.suggest);
    if (!verdict.ok) {
      // For "Report this board": report_form.js sends only report.js's fields from these, never the MAC.
      this.refused = { facts, picked: this.board || "Not asked", verdict: verdict.words };
      throw Object.assign(new Error(verdict.words), { plain: true });
    }
    this.chipWords = verdict.words;
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
    const words = describeBoard(board, build.manifest.build, this.names, build.manifest.ancestors);
    const name = buildName(build.manifest.build.app_set);
    const cancel = (label, cls) => h("button", { class: cls, on: { click: async () => { await this.close(); this.ready(); } } }, label);
    if (store.downloadProblem) {
      // A store set up later would land on leftovers (store.js storeAction), so nothing installs without it.
      return this.show(
        h("p", { class: "install-read" }, words.board),
        words.warning ? h("p", { class: "install-error", role: "alert" }, words.warning) : null,
        h("p", {}, `A download for its store didn't come through: ${store.downloadProblem}. Nothing was written. Try again in a few minutes.`),
        h("div", { class: "row" }, cancel("Close", "alt")),
      );
    }
    // The store's downloads come from GitHub, straight into this browser: M-VAVE's stock firmware and the loader file.
    const storeWords = store.action !== "create" ? ""
      : ` It also sets up a store with ${listOf(store.starter)}, downloaded from GitHub.`;
    // The chip check's words matter only when the visitor picked the board, or nobody has tried this one.
    const chipWords = this.chipWords && (this.boards.length > 1 || choice.tested === false) ? this.chipWords : null;
    const install = h("button", { class: words.newer ? "alt" : null, on: { click: () => this.run(choice, build, board, store) } },
      words.newer ? "Install the older build" : `Install ${name}`);
    this.show(
      h("p", { class: "install-read" }, words.board),
      words.warning ? h("p", { class: "install-error", role: "alert" }, words.warning) : null,
      chipWords ? h("p", {}, chipWords) : null,
      h("p", {}, words.install + storeWords),
      // A newer build stays unless the visitor means to go back: keeping it comes first.
      h("div", { class: "row" }, ...(words.newer ? [cancel("Keep it"), install] : [install, cancel("Cancel", "alt")])),
      this.countBox(),
    );
  }

  progress(label, done, total) {
    const bar = h("progress", { max: total || 1, value: done || 0 });
    this.show(bar, h("p", { class: "install-note", role: "status" }, label, ". Keep the cable in."));
  }

  async run(choice, build, board, store) {
    if (lock.busy) return;
    lock.busy = true;
    let verified = false;
    const label = { verify: "Checking every part", "verify store": "Checking the new store" };
    try {
      const { storeProblem } = await install({
        loader: this.loader, board, manifest: build.manifest, parts: build.parts,
        storeWrites: store.storeWrites, storeWritable: store.storeWritable,
        onStep: (s) => this.progress(label[s.step] || `Writing the ${s.part}`, s.written, s.total),
      });
      // Verified: from here on it's installed, whatever the restart does.
      verified = true;
      this.count(choice);
      const { loader, transport } = this;
      this.transport = this.loader = null;
      await finish({ loader, transport });
      this.done(build, store, storeProblem);
    } catch (e) {
      if (!verified) this.count(choice, "write"); // after the check it counted as installed
      await this.close();
      this.ready(this.explain(e, "The install stopped."));
    } finally {
      lock.busy = false;
    }
  }

  done(build, store, storeProblem) {
    const name = `${buildName(build.manifest.build.app_set)} ${build.manifest.build.commit}`;
    const next = store.action !== "create" ? "Unplug it and plug it into the hub."
      : storeProblem ? `Firmware switching isn't set up: ${storeProblem}.`
        : h("span", {}, "Next: add firmwares in the ", h("a", { href: "/library/" }, "Firmware library"), ".");
    const rst = h("p", { class: "install-note", hidden: true }, "Screen still dark? Tap RST once.");
    setTimeout(() => { rst.hidden = false; }, RST_HINT_MS);
    this.show(h("div", { class: "install-done", role: "status" },
      h("p", {}, h("b", {}, `Installed ${name}.`)),
      h("p", {}, next),
      rst));
  }

  explain(e, lead) {
    const msg = String(e && e.message ? e.message : e);
    if (e && e.plain) return msg; // the chip check's own words (chip.js)
    if (/Nothing was written|didn't read back|download|template/.test(msg)) return `${lead} ${msg}`;
    return `${lead} Check the cable is a data cable and the board is in Computer mode (or BOOT+RST), then try again. (${msg})`;
  }
}

// "A, B and C"
function listOf(names) {
  return names.length < 2 ? names.join("") : `${names.slice(0, -1).join(", ")} and ${names.at(-1)}`;
}

for (const el of document.querySelectorAll(".install[data-builds]")) new InstallArea(el);
