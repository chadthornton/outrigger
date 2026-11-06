// The firmware library page (site/library/index.html): the catalog in three
// tabs, browsable without a board; with an Outrigger in Computer mode, what its
// store holds, and a cart of adds that Apply sends over one connection
// (library.js has the logic, add.js the catalog and the link); on protocol 2,
// removes too, and which package the FM-1 runs. The board stays in
// its running program, and the FM-1 isn't touched: switching happens on the
// board, which backs up first.
import { addableFirmwares, fetchHubPackage, loadCatalog, BoardLink } from "./add.js";
import { applyOutcome, addError, canAdd, countLine, elapsed, libraryRows, meter, queueUpdate, refusedWhy, removable, removeError,
  removeWhy, replacing, room, shownRows, steps, storageLine, toggle } from "./library.js";
import { PORT_FILTER, SERIAL_OPTIONS, port as lock, h } from "./ui.js";

const BAUD = 115200;  // ignored by USB-Serial-JTAG, but Web Serial needs one
const FAVS = "outrigger.favorites";
const RISKIER_CONFIRM = "This firmware can overwrite the small record where the FM-1 keeps its Bluetooth address. " +
  "Your songs and the FM-1 itself are fine, but if you later go back to stock firmware, the FM-1 may get a new " +
  "Bluetooth address: phone pairings would need redoing, and Outrigger will see it as a new FM-1.\n\nAdd it anyway?";

// The {write, read} BoardLink talks over, on an open Web Serial port (as add_area.js).
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

function loadFavs() {
  try { return JSON.parse(localStorage.getItem(FAVS)) || []; } catch { return []; }
}
function saveFavs(favs) {
  try { localStorage.setItem(FAVS, JSON.stringify(favs)); } catch { /* private window: this visit only */ }
}

const mb = (bytes) => `${(bytes / 1048576).toFixed(1)} MB`;
const label = (fw) => `${fw.name} ${fw.version}`.trim();
const firstSentence = (s) => (s.match(/^.*?[.!?](\s|$)/)?.[0] || s).trim();
const dateWords = (d) => {
  const t = Date.parse(d);
  return Number.isNaN(t) ? "" : new Date(t).toLocaleDateString(undefined, { year: "numeric", month: "short", day: "numeric", timeZone: "UTC" });
};
const HEART = '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M12 20.5s-7.5-4.6-9.3-9.2C1.5 8.2 3.6 4.5 7.2 4.5c2 0 3.6 1.1 4.8 2.8 1.2-1.7 2.8-2.8 4.8-2.8 3.6 0 5.7 3.7 4.5 6.8-1.8 4.6-9.3 9.2-9.3 9.2z"/></svg>';

class Library {
  constructor() {
    this.status = document.getElementById("status");
    this.main = document.getElementById("main");
    this.side = document.getElementById("side");
    this.s = {
      tab: "board", q: "", favs: loadFavs(), open: new Set(), cart: [], removes: [], info: null, refused: {},
      board: "none",  // none | steps | talking | connected | silent
      hello: null, stored: null, catalog: null, lists: null, catalogError: null,
      applying: null, result: null, connectError: null,
    };
    this.render();
    this.loadCatalog();
  }

  set(patch) {
    Object.assign(this.s, patch);
    this.render();
  }

  async loadCatalog() {
    try {
      const catalog = await loadCatalog();
      this.set({ catalog, lists: addableFirmwares(catalog) });
    } catch (e) {
      this.set({ catalogError: e.message });
    }
  }

  rows() {
    const { lists, catalog, stored } = this.s;
    if (!lists) return { board: [], available: [], riskier: [], kept: [] };
    const on = this.s.board === "connected";
    return libraryRows(lists, catalog, on ? stored : null, on ? this.s.info?.installed || "" : "");
  }

  // ---------- The board ----------

  async connect() {
    if (lock.busy) return;
    let port;
    try {
      port = await navigator.serial.requestPort({ filters: PORT_FILTER });
    } catch {
      return;  // closed the picker
    }
    lock.busy = true;
    this.set({ board: "talking", connectError: null });
    try {
      await port.open({ baudRate: BAUD, ...SERIAL_OPTIONS });
      this.io = serialIo(port);
      this.link = new BoardLink(this.io);
      const hello = await this.link.hello();
      if (hello.protocol > 2) throw new Error("This Outrigger speaks a newer protocol than this page. Reload the page.");
      const stored = await this.link.list();
      // Protocol 1 boards (before Remove) add only and say nothing about the FM-1.
      const info = hello.protocol >= 2 ? await this.link.info() : null;
      this.set({ board: "connected", hello, stored, info, tab: "board", cart: [], removes: [], refused: {}, result: null });
    } catch (e) {
      await this.close();
      // No answer: an ESP32 without Outrigger, or one not in Computer mode. Over Web Serial they look the same.
      if (/didn't answer/.test(e.message)) this.set({ board: "silent" });
      else this.set({ board: "none", connectError: `Couldn't connect. ${e.message}` });
    } finally {
      lock.busy = false;
    }
  }

  async close() {
    const io = this.io;
    this.io = this.link = null;
    if (io) await Promise.race([io.close().catch(() => {}), new Promise((r) => setTimeout(r, 2000))]);
  }

  // Every add downloaded and checked first, then every queued remove, then
  // every add, in order, over this one connection; stops at the first that
  // fails. A slow step shows how long it has run.
  async apply() {
    if (lock.busy || !this.link) return;
    lock.busy = true;
    const plan = steps(this.s.removes, this.s.cart);
    const results = [];
    const refused = { ...this.s.refused };
    let tick = null;
    const step = (words, got = 0, total = 0) => this.set({ applying: { step: words, got, total, words, since: Date.now() } });
    try {
      tick = setInterval(() => {
        const a = this.s.applying;
        if (a && !a.total) { a.step = elapsed(a.words, Date.now() - a.since); this.renderSide(); }
      }, 1000);
      const data = new Map();
      for (const fw of this.s.cart) {
        step(`Downloading ${label(fw)} from ${this.s.catalog.from}…`);
        try {
          data.set(fw, await fetchHubPackage(fw, fetch, this.s.catalog.from));
        } catch (e) {
          this.set({ applying: null, result: { error: e, names: [] } });
          return;
        }
      }
      for (const st of plan) {
        if (st.remove) {
          const name = this.nameOf(st.remove);
          step(`Removing ${name}…`);
          try {
            await this.link.remove(st.remove);
            delete refused[st.remove];
            results.push("removed");
          } catch (e) {
            // The FM-1 needs it: nothing changed, so the rest go on, and its row says why.
            if (refusedWhy(e)) { refused[st.remove] = refusedWhy(e); results.push("kept"); continue; }
            results.push(Object.assign(e, { message: removeError(e, name) }));
            break;
          }
          continue;
        }
        const fw = st.add;
        const bytes = data.get(fw);
        step(`Sending ${label(fw)} to Outrigger. Keep the cable in.`, 0, bytes.length);
        try {
          // The last data frame's answer waits for the store: show that while it does.
          const storing = `Outrigger is storing ${label(fw)}…`;
          results.push(await this.link.add(fw, bytes, (got, total) => {
            if (got + this.link.dataMax >= total) { if (this.s.applying.words !== storing) step(storing); }
            else { this.s.applying.got = got; this.renderSide(); }
          }));
        } catch (e) {
          results.push(Object.assign(e, { message: addError(e, label(fw)) }));
          break;
        }
      }
      const out = applyOutcome(plan, results);
      const names = [...out.landed.map((st) => (st.remove ? `removed ${this.nameOf(st.remove)}` : `added ${label(st.add)}`)),
        ...out.kept.map((st) => `kept ${this.nameOf(st.remove)} (its row says why)`)];
      const left = { removes: out.left.filter((st) => st.remove).map((st) => st.remove), cart: out.left.filter((st) => st.add).map((st) => st.add) };
      try {
        const hello = await this.link.hello();
        const stored = await this.link.list();
        const info = hello.protocol >= 2 ? await this.link.info() : null;
        this.set({ ...left, refused, applying: null, hello, stored, info, result: { error: out.error, names } });
      } catch {
        await this.close();
        this.set({ ...left, refused, applying: null, board: "none", hello: null, stored: null, info: null, result: { error: out.error, names },
          connectError: "Outrigger's connection closed. Connect again to see what it has." });
      }
    } finally {
      clearInterval(tick);
      lock.busy = false;
    }
  }

  // A stored package's name for messages: its row's, else its id.
  nameOf(id) {
    const r = this.rows().board.find((x) => x.key === id);
    return r ? `${r.name} ${r.version}`.trim() : id;
  }

  unqueueRemove(id) {
    this.set({ removes: this.s.removes.filter((x) => x !== id), result: null });
  }

  queueRemove(id) {
    this.set({ removes: [...this.s.removes, id], result: null });
  }

  update(row) {
    this.set({ ...queueUpdate(this.s, row), result: null });
  }

  queue(fw) {
    if (fw.riskier && !this.s.cart.includes(fw) && !window.confirm(RISKIER_CONFIRM)) return;
    const cart = this.s.cart.includes(fw) ? this.s.cart.filter((f) => f !== fw) : [...this.s.cart, fw];
    this.set({ cart, result: null });
  }

  askToConnect() {
    if (!("serial" in navigator) || this.s.board === "talking") return;
    this.set({ board: "steps", connectError: null });
    this.main.scrollIntoView({ behavior: "smooth" });
  }

  // ---------- Drawing ----------

  render() {
    this.renderStatus();
    this.renderMain();
    this.renderSide();
  }

  renderStatus() {
    const { board } = this.s;
    if (board === "connected") return this.status.replaceChildren(h("span", { class: "pill" }, h("i"), "Outrigger connected"));
    if (!("serial" in navigator)) {
      return this.status.replaceChildren(h("span", { class: "plug" }, h("span", {}, "Adding firmwares needs Chrome or Edge on a computer")));
    }
    this.status.replaceChildren(h("span", { class: "plug" },
      h("span", {}, board === "talking" ? "Talking to Outrigger…" : "Plug in your Outrigger"),
      h("button", { class: "btn", disabled: board === "talking", on: { click: () => this.set({ board: "steps", connectError: null }) } }, "Connect")));
  }

  // Connecting or applying: one job on the board at a time, so nothing else on the page answers.
  busy() {
    return this.s.board === "talking" || !!this.s.applying;
  }

  renderMain() {
    const s = this.s;
    const rows = this.rows();
    this.main.toggleAttribute("aria-busy", s.board === "talking");
    this.main.toggleAttribute("inert", s.board === "talking");
    const tab = (id, text, cls) => h("button", { role: "tab", class: cls, "aria-selected": String(s.tab === id),
      on: { click: () => this.set({ tab: id }) } }, text);
    this.main.replaceChildren(...[
      s.board === "steps" ? this.stepsBox() : null,
      s.board === "silent" ? this.silentBox() : null,
      s.connectError ? h("div", { class: "box" }, h("p", { class: "error", role: "alert" }, s.connectError)) : null,
      s.board === "connected" ? this.meterBox(rows) : null,
      h("nav", { class: "tabs", role: "tablist", "aria-label": "Library sections" },
        tab("board", "On your board"), tab("available", "Available"), tab("riskier", "Riskier"), tab("about", "About these", "about")),
      ...this.tabBody(rows),
    ].filter(Boolean));
  }

  stepsBox() {
    const shot = (src, alt, cap) => h("figure", {}, h("img", { src, alt }), h("figcaption", {}, cap));
    return h("section", { class: "box", "aria-label": "Connect your Outrigger" },
      h("h2", {}, "Connect your Outrigger"),
      h("ol", { class: "steps" },
        h("li", {}, "On Outrigger, open Settings, then Computer mode, and hold the button."),
        h("li", {}, "Plug it into this computer with a data cable.")),
      h("div", { class: "shots" },
        shot("../shots/M_setup.png", "Outrigger's Settings, with Computer mode", "Settings"),
        shot("../shots/M_computer_confirm.png", "Computer mode screen: hold to restart in Computer mode", "Hold to restart")),
      h("div", {}, h("button", { class: "btn", on: { click: () => this.connect() } }, "Choose my Outrigger")),
      h("p", {}, "No Outrigger on it yet? ", h("a", { href: "../#install" }, "Install it first"), ": about 2 minutes."));
  }

  silentBox() {
    return h("section", { class: "box banner", "aria-label": "Install Outrigger first" },
      h("div", {},
        h("h2", {}, "That port isn't Outrigger in Computer mode"),
        h("p", {}, "No Outrigger on it yet? Install it, then come back. Already installed? Open Settings, then Computer mode, and try again.")),
      h("a", { class: "btn", href: "../#install" }, "Install Outrigger"),
      h("button", { class: "btn ghost", on: { click: () => this.set({ board: "steps" }) } }, "Try again"));
  }

  meterBox(rows) {
    const { hello, cart, removes } = this.s;
    const cells = meter(hello, rows.kept.length, cart, removes);
    const names = { kept: "Always kept", installed: "On your board", removing: "Being removed", cart: "In your changes", free: "Free",
      full: "No room in memory" };
    const key = (cls, text) => h("span", {}, h("i", { class: cls }), text);
    return h("section", { class: "box", "aria-label": "Storage" },
      h("div", { class: "meterhead" }, h("h2", {}, "Your Outrigger's firmware slots"), h("span", {}, storageLine(hello, cart, removes))),
      h("div", { class: "cells", "aria-hidden": "true" }, cells.map((c) => h("i", { class: `c-${c}`, title: names[c] }))),
      h("div", { class: "key" },
        key("c-kept", "Always kept: stock V15"), key("c-installed", "On your board"), key("c-cart", "In your changes"),
        removes.length ? key("c-removing", "Being removed") : null, key("c-free", "Free"),
        cells.includes("full") ? key("c-full", "No room in memory") : null));
  }

  tabBody(rows) {
    const s = this.s;
    if (s.tab === "about") return [this.aboutBox()];
    if (s.catalogError && s.tab !== "board") return [h("div", { class: "box" }, h("p", { class: "error", role: "alert" }, s.catalogError))];
    if (!s.lists && s.tab !== "board") return [h("div", { class: "box" }, h("p", { role: "status" }, "Getting the list from mutant.cloud…"))];
    if (s.tab === "available") {
      const shown = shownRows(rows.available, s.q, s.favs);
      this.listEl = h("div", { class: "list", role: "list" });
      this.countEl = h("span", { role: "status" });
      const input = h("input", { id: "fw-search", type: "search", value: s.q, placeholder: "Search by name, author or what it does",
        on: { input: (e) => { this.s.q = e.target.value; this.renderAvailable(); } } });
      const out = [h("div", { class: "search" }, h("label", { for: "fw-search", class: "sr" }, "Search firmwares"), input, this.countEl), this.listEl];
      this.renderAvailable(shown);
      return out;
    }
    if (s.tab === "riskier") {
      return [this.riskierBox(), this.list(rows.riskier, "Nothing here right now.")];
    }
    // On your board
    if (s.board !== "connected") {
      return [h("div", { class: "list" }, h("p", { class: "empty" }, "Connect your Outrigger to see the firmwares on it. ",
        "Or look through ", h("a", { href: "#", on: { click: (e) => { e.preventDefault(); this.set({ tab: "available" }); } } }, "Available"), "."))];
    }
    return [this.list(rows.board, "Only the always-kept firmwares so far. Add some from Available."), this.keptBox(rows.kept)];
  }

  renderAvailable(shown) {
    const rows = this.rows().available;
    shown ||= shownRows(rows, this.s.q, this.s.favs);
    this.countEl.textContent = countLine(shown.length, rows.length, this.s.q);
    this.listEl.replaceChildren(...(shown.length ? shown.map((r) => this.row(r)) : [h("p", { class: "empty" }, "No firmware matches that.")]));
  }

  list(rows, empty) {
    return h("div", { class: "list", role: "list" }, rows.length ? rows.map((r) => this.row(r)) : h("p", { class: "empty" }, empty));
  }

  // One firmware: name and version, one short note, and only the actions that apply.
  row(r) {
    const s = this.s;
    const queued = r.fw && s.cart.includes(r.fw);
    const connected = s.board === "connected";
    const full = connected && !canAdd(s.hello, s.cart, s.removes);
    const stop = (fn) => (e) => { e.preventDefault(); e.stopPropagation(); fn(); };
    // Without a board, Add asks for one: the room it has decides what fits.
    const busy = this.busy();
    const go = (text, fn = () => this.queue(r.fw), more = []) => h("button", { class: "go", disabled: (full && !canAdd(s.hello, s.cart, more)) || busy,
      title: full ? "Outrigger has no room for more" : null,
      on: { click: stop(() => (connected ? fn() : this.askToConnect())) } }, text);
    const undo = h("button", { class: "undo", disabled: busy, on: { click: stop(() => (r.state === "update" ? this.update(r) : this.queue(r.fw))) } }, "Undo");
    // Remove: protocol 2 boards only, and never what the FM-1 runs or stock.
    const canRemove = connected && s.info !== null && removable(r);
    const removing = s.removes.includes(r.key);
    const rm = canRemove ? h("button", { class: "undo", disabled: busy, on: { click: stop(() => this.queueRemove(r.key)) } }, "Remove") : null;
    // Undoing a remove whose place the cart has taken would leave an add the board refuses.
    const taken = connected && s.cart.length > room(s.hello, s.removes.length - 1);
    const keep = h("button", { class: "undo", disabled: busy || taken,
      title: taken ? "An add in your changes uses its place: undo that first" : null,
      on: { click: stop(() => this.unqueueRemove(r.key)) } }, "Undo");
    let note = "", tone = "", acts = [];
    const why = connected ? removeWhy(r, s.refused) : null;
    if (r.state === "update" && replacing(r, s.cart, s.removes)) {
      note = `Will update to ${r.fw.version}, replacing this copy`;
      tone = "warn";
      acts = [undo];
    } else if (removing) {
      note = "Will be removed. Its songs stay saved on the board.";
      tone = "warn";
      acts = [keep];
    } else if (r.running) {
      note = r.state === "update" && queued ? `Will add ${r.fw.version}. This copy stays: your FM-1 runs it.` : why;
      tone = "ok";
      if (r.state === "update") acts = [queued ? undo : go("Update", () => this.update(r))];
    } else if (why) {
      note = why;
      tone = "warn";
    } else if (r.state === "current") {
      note = "On your Outrigger";
      acts = [rm];
    } else if (r.state === "update") {
      note = queued ? `Will add ${r.fw.version}. This copy stays.` : `Update available: ${r.fw.version}`;
      tone = "warn";
      acts = [queued ? undo : go("Update", () => this.update(r), [...s.removes, r.key]), rm];
    } else if (r.state === "old") {
      note = "An older version: the newest is on your Outrigger too";
      acts = [rm];
    } else if (r.state === "unknown") {
      note = "Added outside this page";
      acts = [rm];
    }
    else if (r.state === "available") {
      note = queued ? "Will be added" : firstSentence(r.what);
      acts = [queued ? undo : go("Add")];
    } else if (r.state === "riskier") {
      note = queued ? "Will be added" : "May change your Bluetooth address";
      tone = queued ? "" : "warn";
      acts = [queued ? undo : go("Add…")];
    } else if (r.state === "soon") {
      note = "May change your Bluetooth address";
      tone = "warn";
      acts = [h("span", { class: "soon" }, "Coming soon")];
    } else if (r.state === "held") {
      note = /Bluetooth/.test(r.why) ? "Held back: changes the FM-1's Bluetooth record" : "Held back: effects unknown";
      tone = "warn";
    }
    const fav = s.favs.includes(r.key);
    const heart = s.tab === "available" ? h("button", { class: "heart", "aria-pressed": String(fav),
      "aria-label": fav ? `Remove ${r.name} from favorites` : `Add ${r.name} to favorites`,
      on: { click: stop(() => { const favs = toggle(s.favs, r.key); saveFavs(favs); this.s.favs = favs; this.renderAvailable(); }) } }) : null;
    if (heart) heart.innerHTML = HEART;

    const d = h("details", { role: "listitem", class: queued ? "queued" : removing ? "removing" : null, open: s.open.has(r.key) },
      h("summary", {}, heart,
        h("div", { class: "who" }, h("span", {}, h("b", {}, r.name), r.version ? " " : null, r.version ? h("span", { class: "ver" }, r.version) : null),
          note ? h("span", { class: `note ${tone}` }, note) : null),
        acts.some(Boolean) ? h("span", { class: "acts" }, acts) : null),
      this.more(r));
    d.addEventListener("toggle", () => { if (d.open) s.open.add(r.key); else s.open.delete(r.key); });
    return d;
  }

  more(r) {
    const songs = r.state === "held" ? "Not installable yet."
      : ["current", "update", "old", "unknown"].includes(r.state)
        ? "Every switch on Outrigger backs up the FM-1 first, so songs you made on this firmware come back when you switch back to it."
        : r.state === "riskier" || r.state === "soon"
          ? "Switching to it saves your current firmware's songs first. Going back to stock later may give the FM-1 a new Bluetooth address."
          : "Switching to it saves your current firmware's songs first.";
    // The catalog is someone else's: only a web link goes in an href.
    const news = /^https?:\/\//.test(r.release) ? h("span", {}, h("a", { href: r.release, target: "_blank", rel: "noopener" }, "Release notes"),
      r.date ? ` · ${dateWords(r.date)}` : "") : r.date ? `Released ${dateWords(r.date)}` : null;
    return h("div", { class: "more" },
      r.why ? h("p", {}, r.why.replace(/^Not yet: /, "Held back: ")) : r.what ? h("p", {}, r.what) : null,
      h("dl", {},
        h("dt", {}, "Your songs"), h("dd", {}, songs),
        news ? [h("dt", {}, "What's new"), h("dd", {}, news)] : null,
        r.author ? [h("dt", {}, "Made by"), h("dd", {}, r.author)] : null));
  }

  keptBox(kept) {
    const v = kept[0]?.version || "V15";
    return h("section", { class: "kept", "aria-label": "Always kept" },
      h("h3", {}, "Always kept"),
      h("div", { class: "box" },
        h("span", {}, h("b", {}, "M-VAVE stock"), " ", h("span", { class: "ver" }, v)),
        h("span", { class: "note" }, "The factory firmware, plus a backup of every firmware your FM-1 has run. Kept so you can always go back."),
        h("span", { class: "note" }, "M-VAVE has released V16. Outrigger stays on V15 for now: V16 replaces the FM-1's start-up code, and after that Outrigger can only back it up.")));
  }

  riskierBox() {
    return h("div", { class: "box" },
      h("p", { class: "lead" }, "These can't break your FM-1, but they can change something Outrigger can't put back yet."),
      h("p", {}, "They overwrite a small record where the FM-1 keeps its Bluetooth address. Your songs and the FM-1 itself are fine. " +
        "The catch: if you later go back to stock firmware, the FM-1 may get a new Bluetooth address. Phone pairings would then need redoing, " +
        "and Outrigger will see it as a new FM-1."),
      h("p", {}, h("b", {}, "If something goes wrong:"), " Outrigger backs up your FM-1 before every install, so you can switch back from the " +
        "board's Firmware screen in about a minute. If the FM-1 won't start, leave it plugged in and switched on, and the board finishes or " +
        "retries the install. The full steps are in ", h("a", { href: "../#trouble" }, "If your FM-1 won't boot"), "."),
      h("p", {}, "We're working on a fix that restores the Bluetooth record after a switch. When it ships, these move to Available."));
  }

  aboutBox() {
    return h("section", { class: "box", "aria-label": "How installs stay safe" },
      h("h2", {}, "How installs stay safe"),
      h("p", {}, "Before it changes anything, Outrigger backs up your FM-1 and checks the backup. It never touches the part of the FM-1 " +
        "that lets it recover. If an install is cut short, leave the FM-1 on and the board picks up where it stopped."),
      h("p", {}, "These firmwares are made by their own authors. Outrigger checks that it can install them and switch you back; how each one plays is up to its author."),
      h("p", {}, "Firmwares under ", h("b", {}, "Riskier"), " change something on the FM-1 that Outrigger can't put back yet. " +
        "They won't break it, and that tab says exactly what changes. You confirm before installing one."),
      this.s.catalog ? h("p", {}, `The list is from ${this.s.catalog.from}. Every file is downloaded straight into your browser and checked before it goes to Outrigger.`) : null);
  }

  renderSide() {
    const s = this.s;
    const parts = [];
    if (s.board === "connected") parts.push(this.outriggerBox());
    if (s.cart.length || s.removes.length || s.applying || s.result) parts.push(this.cartBox());
    parts.push(h("section", { class: "box", "aria-label": "Need more space" },
      h("h2", {}, "Need more space?"),
      h("p", {}, "Outrigger keeps up to 10 firmwares in its own memory, stock included. Support for a microSD card, on boards with a slot, is on the way."),
      h("a", { href: "../#board" }, "Boards with a slot")));
    this.side.replaceChildren(...parts);
  }

  // What 'H' says.
  outriggerBox() {
    const { hello } = this.s;
    return h("section", { class: "box", "aria-label": "Your Outrigger" },
      h("h2", {}, "Your Outrigger"),
      h("dl", { class: "facts" },
        h("dt", {}, "Firmwares"), h("dd", {}, `${hello.packages} of ${hello.maxPackages}`),
        h("dt", {}, "Free memory"), h("dd", {}, mb(hello.free)),
        h("dt", {}, "Build"), h("dd", {}, hello.build.replace(/^v /, ""))));
  }

  // The slot for PR 4's "Done: back to MIDI" (protocol 3: a frame that
  // restarts the board into Play). connect() refuses protocol > 2 until then.
  doneButton() {
    return null;
  }

  cartBox() {
    const s = this.s;
    const total = s.cart.reduce((n, f) => n + (f.size || 0), 0);
    const count = s.cart.length + s.removes.length;
    const r = s.result;
    const did = r && r.names.length ? `${r.names.join(", ").replace(/^./, (c) => c.toUpperCase())}. ` : "";
    return h("section", { class: "box", "aria-label": "Your changes" },
      h("h2", {}, "Your changes"),
      count ? h("div", { class: "cart" },
        s.removes.map((id) => h("div", {}, h("span", {}, `Remove ${this.nameOf(id)}`), h("span", {}, ""))),
        s.cart.map((f) => h("div", {},
          h("span", {}, f.name, " ", h("span", { class: "ver" }, f.version)), h("span", {}, f.size ? mb(f.size) : "")))) : null,
      r && r.error ? h("p", { class: "error", role: "alert" }, did + r.error.message) : null,
      r && !r.error ? [h("p", { class: "done", role: "status" }, `${did}Done.`), this.doneButton()] : null,
      s.applying ? [h("p", { role: "status" }, s.applying.step),
        s.applying.total ? h("progress", { max: s.applying.total, value: s.applying.got }) : h("progress")] : null,
      count && !s.applying ? [
        h("p", {}, [
          s.cart.length ? `Adds use about ${mb(total)}. ` : "",
          s.removes.length ? "Removed firmwares keep their saved songs on the board. " : "",
          "Nothing here touches your FM-1: the switch, on the board's Firmware screen, backs it up first. ",
          "Every change goes over this USB connection in one go.",
        ].join("")),
        h("button", { class: "btn", disabled: s.board !== "connected", on: { click: () => this.apply() } },
          `Apply ${count} ${count === 1 ? "change" : "changes"}`)] : null);
  }
}

new Library();
