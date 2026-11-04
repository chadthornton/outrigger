// The "Add a firmware" area on site/index.html: connect to an Outrigger in
// Computer mode, list the Firmware Hub's firmwares against what its store
// has, and add one (add.js does the work). The board stays in its running
// program: no BOOT+RST, nothing but its store's new package is written.
//
//   <div class="add-firmware"></div>
import { addableFirmwares, fetchHubPackage, BoardLink, CATALOG_URL } from "./add.js";
import { PORT_FILTER, SERIAL_OPTIONS, port as lock, h } from "./ui.js";

const BAUD = 115200;  // ignored by USB-Serial-JTAG, but Web Serial needs one
const HOW = "On Outrigger, open Settings, then Computer mode, and hold the button. Then plug it into this computer with a data cable.";

// The {write, read} BoardLink talks over, on an open Web Serial port.
function serialIo(port) {
  const writer = port.writable.getWriter();
  const reader = port.readable.getReader();
  return {
    async write(bytes) { await writer.write(bytes); },
    async read() {
      const { value, done } = await reader.read();
      return done ? undefined : value;
    },
    async close() {
      try { await reader.cancel(); } catch { /* gone */ }
      reader.releaseLock();
      writer.releaseLock();
      await port.close();
    },
  };
}

const mb = (bytes) => `${(bytes / 1048576).toFixed(1)} MB`;

class AddArea {
  constructor(el) {
    this.el = el;
    this.ready();
  }

  show(...kids) {
    this.el.replaceChildren(...kids.filter((k) => k != null));
  }

  ready(note) {
    if (!("serial" in navigator)) {
      return this.show(h("p", { class: "install-note" }, "Adding a firmware needs Chrome or Edge on a computer."));
    }
    this.show(
      note ? h("p", { class: "install-error", role: "alert" }, note) : null,
      h("div", { class: "row" }, h("button", { on: { click: () => this.steps() } }, "Connect to my Outrigger")),
    );
  }

  // What to do on the board first, with the two screens to look for; the port picker comes after.
  steps() {
    const shot = (src, alt, cap) => h("figure", {}, h("img", { src, alt }), h("figcaption", {}, cap));
    this.show(
      h("ol", { class: "add-steps" },
        h("li", {}, "On Outrigger, open Settings, then Computer mode, and hold the button."),
        h("li", {}, "Plug it into this computer with a data cable.")),
      h("div", { class: "shots-sm" },
        shot("shots/M_setup.png", "Outrigger's Settings, with Computer mode", "Settings"),
        shot("shots/M_computer_confirm.png", "Computer mode screen: hold to restart in Computer mode", "Hold to restart")),
      h("div", { class: "row" }, h("button", { on: { click: () => this.connect() } }, "Choose my Outrigger")),
    );
  }

  async connect() {
    if (lock.busy) return;
    let port;
    try {
      port = await navigator.serial.requestPort({ filters: PORT_FILTER });
    } catch {
      return;  // closed the picker
    }
    lock.busy = true;
    try {
      this.show(h("p", { class: "install-note", role: "status" }, "Talking to Outrigger…"));
      await port.open({ baudRate: BAUD, ...SERIAL_OPTIONS });
      this.io = serialIo(port);
      this.link = new BoardLink(this.io);
      const hello = await this.link.hello();
      if (hello.protocol !== 1) throw new Error("This Outrigger speaks a newer protocol than this page. Reload the page.");
      const res = await fetch(CATALOG_URL, { cache: "no-cache" });
      if (!res.ok) throw new Error(`The Firmware Hub answered ${res.status} for its list.`);
      this.catalog = addableFirmwares(await res.json());
      await this.list();
    } catch (e) {
      await this.close();
      this.ready(/Computer mode/.test(e.message) ? `Outrigger didn't answer. ${HOW} If it has never been updated from this page, install first (above), then try again.`
        : `Couldn't connect. ${e.message}`);
    } finally {
      lock.busy = false;
    }
  }

  async list(note) {
    const hello = await this.link.hello();
    const stored = new Set((await this.link.list()).map((p) => p.sha256));
    const full = hello.packages >= hello.maxPackages;
    const rows = this.catalog.addable.map((fw) => h("li", {},
      h("span", {}, h("b", {}, fw.name), ` ${fw.version}`),
      stored.has(fw.sha256) ? h("small", {}, "On your Outrigger")
        : h("button", { class: "alt", disabled: full, on: { click: () => this.add(fw) } }, "Add")));
    this.show(
      note ? h("p", { class: note.bad ? "install-error" : "install-done", role: note.bad ? "alert" : "status" }, note.text) : null,
      h("p", { class: "install-read" }, `Outrigger has ${hello.packages} of ${hello.maxPackages} firmwares, ${mb(hello.free)} free.`,
        full ? " Its list is full." : ""),
      h("ul", { class: "add-list" }, rows),
      this.catalog.held.length ? h("details", { class: "install-log" }, h("summary", {}, `Not on the list yet (${this.catalog.held.length})`),
        h("ul", {}, this.catalog.held.map((f) => h("li", {}, h("b", {}, f.name), ` ${f.why}`)))) : null,
      h("div", { class: "row" }, h("button", { class: "alt", on: { click: async () => { await this.close(); this.ready(); } } }, "Done")),
    );
  }

  async add(fw) {
    if (lock.busy) return;
    lock.busy = true;
    const label = `${fw.name} ${fw.version}`.trim();
    try {
      this.show(h("p", { class: "install-note", role: "status" }, `Downloading ${label} from the Firmware Hub…`));
      const data = await fetchHubPackage(fw);
      const bar = h("progress", { max: data.length, value: 0 });
      const words = h("p", { class: "install-note", role: "status" }, `Sending ${label} to Outrigger. Keep the cable in.`);
      this.show(bar, words);
      const result = await this.link.add(fw, data, (got, total) => {
        bar.value = got;
        if (got === total) words.textContent = `Outrigger is storing ${label}…`;
      });
      await this.list({ text: result === "already" ? `${label} was already on Outrigger.`
        : `Added ${label}. Press Back to MIDI on Outrigger, then find it on the FM-1 page.` });
    } catch (e) {
      try {
        await this.list({ bad: true, text: `${label} wasn't added. ${e.message}` });
      } catch {
        await this.close();
        this.ready(`${label} wasn't added. ${e.message}`);
      }
    } finally {
      lock.busy = false;
    }
  }

  async close() {
    const io = this.io;
    this.io = this.link = null;
    if (io) await Promise.race([io.close().catch(() => {}), new Promise((r) => setTimeout(r, 2000))]);
  }
}

for (const el of document.querySelectorAll(".add-firmware")) new AddArea(el);
