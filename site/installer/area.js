// The install area under each tab on site/index.html: its buttons, and the
// four states it moves through in place (ready, board read, installing,
// done). No dialog: the only pop-up is the browser's own port picker.
//
//   <div class="install" data-builds='[{"label":"Install","manifest":"firmware/.../hub/manifest.json"}]'></div>
//
// Without JavaScript, or in a browser without Web Serial, the area says so and
// the text around it still reads on its own.
import { ESPLoader, Transport } from "../vendor/esptool-js-0.7.0/bundle.js";
import { describeBoard, buildName } from "./board.js";
import { readBoard, fetchBuild, install, backupStore, restart } from "./install.js";

// The ESP32-S3's own USB port (USB-Serial-JTAG), in Computer mode or BOOT+RST.
const PORT_FILTER = [{ usbVendorId: 0x303a, usbProductId: 0x1001 }];
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
    this.busy = false;
    this.ready();
  }

  show(...kids) {
    this.el.replaceChildren(...kids);
  }

  ready(note) {
    if (!("serial" in navigator)) {
      return this.show(h("p", { class: "install-note" }, window.isSecureContext
        ? "This browser can't install. Use Chrome or Edge on a computer."
        : "Installing needs a secure page: open this over https:// or from localhost."));
    }
    this.show(
      note ? h("p", { class: "install-error", role: "alert" }, note) : null,
      h("div", { class: "row" }, this.builds.map((b, i) =>
        h("button", { class: i ? "alt" : null, on: { click: () => this.connect(b) } }, b.label))),
      h("p", { class: "install-note" }, KEEPS, " ", h("a", { href: this.helpHref }, "No board listed?")),
    );
  }

  async connect(choice) {
    if (this.busy) return;
    let port;
    try {
      port = await navigator.serial.requestPort({ filters: PORT_FILTER });
    } catch {
      return; // closed the picker
    }
    this.busy = true;
    this.show(h("p", { class: "install-note", role: "status" }, "Reading the board…"));
    try {
      const [build] = await Promise.all([fetchBuild(choice.manifest), this.open(port)]);
      const board = await readBoard(this.loader);
      this.confirm(choice, build, board);
    } catch (e) {
      await this.close();
      this.ready(this.explain(e, "Couldn't read the board."));
    } finally {
      this.busy = false;
    }
  }

  async open(port) {
    this.transport = new Transport(port);
    this.loader = new ESPLoader({ transport: this.transport, baudrate: 921600, romBaudrate: 115200, terminal: null });
    const chip = await this.loader.main();
    if (!/ESP32-S3/.test(chip)) throw new Error(`That's an ${chip}, not the Waveshare's ESP32-S3.`);
  }

  async close() {
    try { await this.transport?.disconnect(); } catch { /* already gone */ }
    this.transport = this.loader = null;
  }

  confirm(choice, build, board) {
    const words = describeBoard(board, build.manifest.build);
    const save = board.store
      ? h("label", { class: "check" }, h("input", { type: "checkbox", id: "save-store", checked: true }),
        ` Save a copy of my backups to this computer first (${mb(board.storeRange.size)} MB)`)
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
    if (this.busy) return;
    this.busy = true;
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
      await restart(this.loader);
      await this.close();
      this.done(build);
    } catch (e) {
      await this.close();
      this.ready(this.explain(e, "The install stopped."));
    } finally {
      this.busy = false;
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
    if (/Nothing was written|didn't read back|didn't download|not the Waveshare/.test(msg)) return `${lead} ${msg}`;
    return `${lead} Check the cable is a data cable and the board is in Computer mode (or BOOT+RST), then try again. (${msg})`;
  }
}

for (const el of document.querySelectorAll(".install[data-builds]")) new InstallArea(el);
