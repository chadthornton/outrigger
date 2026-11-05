// The "Report this board" pop-up: opens from an install area (area.js) when
// the chip check finds no profile for a board, or refuses its chip. It shows
// what the installer read, takes the board's name, a note and an optional
// email, and posts them to /api/report (functions/api/report.js). What goes
// out is built by report.js, from a fixed list of fields: never the MAC.
import { buildReport, buildLogReport, validateReport, boardFits, LIMITS } from "./report.js";
import { flashBytesFromId, psramMbFromFeatures, USB_BRIDGES } from "./chip.js";
import { h } from "./ui.js";

const FEEDBACK = "feedback@outrigger.music";

function usbWords(vid) {
  if (vid == null) return "didn't say";
  const hex = `0x${vid.toString(16).padStart(4, "0")}`;
  if (vid === 0x303a) return `the chip's own USB (${hex})`;
  return USB_BRIDGES[vid] ? `${USB_BRIDGES[vid]} (${hex})` : hex;
}

// "Request support for my ESP32 board": read it from here, or list it by hand.
export function openRequestChoice({ installer, onRead }) {
  const close = () => dialog.close();
  const dialog = h("dialog", { class: "report", "aria-label": "Request support for my ESP32 board", on: { close: () => dialog.remove() } },
    h("div", { class: "report-choice" },
      h("h2", {}, "Request support for your ESP32 board"),
      onRead ? h("p", {}, h("b", {}, "Have the board?"), " Plug it in and we'll read it for you.") : null,
      onRead ? h("div", { class: "row" }, h("button", { type: "button", on: { click: () => { close(); onRead(); } } }, "Plug it in and read it")) : null,
      h("p", {}, h("b", {}, "Don't have it yet?"), " List it here: its name or a link, a note, and an email if you want to hear back."),
      h("div", { class: "row" },
        h("button", { type: "button", class: onRead ? "alt" : null, on: { click: () => { close(); openReportForm({ facts: null, picked: "", verdict: "Listed by hand, no board read", installer }); } } }, "List it here"),
        h("button", { type: "button", class: "alt", on: { click: close } }, "Cancel"))));
  document.body.append(dialog);
  dialog.showModal();
}

// "Send this log to Outrigger", from a failed install (area.js): shows exactly what goes, takes an optional note
// and email, and posts it to /api/report with the log. {log, error, picked, installer}: area.js's progress lines,
// the words it showed, the board picked, the installer version.
export function openLogForm({ log, error, picked, installer }) {
  const preview = buildLogReport({ log, error, picked, installer, note: "", email: "" });
  const note = h("textarea", { name: "note", rows: 3, maxlength: LIMITS.note, placeholder: "Optional: what you were doing, what you saw" });
  const email = h("input", { name: "email", type: "email", maxlength: LIMITS.email, autocomplete: "email", placeholder: "you@example.com" });
  const field = (label, input, help) => h("label", { class: "report-field" }, h("span", {}, label), input, help ? h("small", {}, help) : null);
  const status = h("p", { class: "report-status", role: "status" });
  const send = h("button", { type: "submit" }, "Send");
  const form = h("form", { method: "dialog", on: { submit: (e) => { e.preventDefault(); submit(); } } },
    h("h2", {}, "Send this log to Outrigger"),
    h("p", {}, "It shows each step the installer took and where it stopped, so we can see what went wrong. This is " +
      "everything that's sent. Your board's MAC address is taken out."),
    h("pre", { class: "report-log" }, [`Board picked: ${preview.picked || "none"}`, `Installer ${preview.installer}`,
      `The page said: ${preview.verdict}`, "", preview.log].join("\n")),
    field("Anything else we should know?", note),
    field("Your email (optional)", email, `So we can reply. Used for nothing else; we'll delete it if you ask (${FEEDBACK}).`),
    status,
    h("div", { class: "row" }, send, h("button", { type: "button", class: "alt", on: { click: () => dialog.close() } }, "Cancel")));
  const dialog = h("dialog", { class: "report", "aria-label": "Send this log to Outrigger", on: { close: () => dialog.remove() } }, form);
  document.body.append(dialog);
  dialog.showModal();

  async function submit() {
    const report = buildLogReport({ log, error, picked, installer, note: note.value, email: email.value });
    const check = validateReport(report);
    if (!check.ok) {
      status.textContent = check.problems.join(" ");
      return;
    }
    send.disabled = true;
    status.textContent = "Sending…";
    let res;
    try {
      res = await fetch("/api/report", { method: "POST", headers: { "content-type": "application/json" }, body: JSON.stringify(report) });
    } catch {
      res = null;
    }
    if (res && res.ok) {
      return form.replaceChildren(h("h2", {}, "Thanks!"),
        h("p", {}, report.email ? `We'll look at it and write to ${report.email}.` : "We'll look at it."),
        h("div", { class: "row" }, h("button", { type: "button", on: { click: () => dialog.close() } }, "Close")));
    }
    send.disabled = false;
    status.textContent = `${res?.status === 429 ? "Too many tries from here. Try again in an hour" : "It didn't send"}, or email ${FEEDBACK}.`;
  }
}

// {facts, picked, verdict, installer}: what the area read and decided. facts null: listed by hand, no chip data.
export function openReportForm({ facts, picked, verdict, installer }) {
  const listed = facts == null;
  if (listed) facts = { chip: "", features: [], flashId: null, usbVendorId: null };
  const flash = flashBytesFromId(facts.flashId);
  const psram = psramMbFromFeatures(facts.features);
  const read = [
    ["Chip", facts.chip || "didn't say"],
    ["Flash", flash === null ? "didn't say" : `${flash / 1048576} MB`],
    ["PSRAM", psram ? `${psram} MB` : "none"],
    ["USB", usbWords(facts.usbVendorId)],
  ];
  const field = (label, input, help) => h("label", { class: "report-field" }, h("span", {}, label), input,
    help ? h("small", {}, help) : null);
  const board = h("input", { name: "board", required: true, maxlength: LIMITS.board, autocomplete: "off",
    placeholder: "Its name, or a link to where you bought it" });
  const note = h("textarea", { name: "note", rows: 3, maxlength: LIMITS.note,
    placeholder: "Optional" });
  const email = h("input", { name: "email", type: "email", maxlength: LIMITS.email, autocomplete: "email",
    placeholder: "you@example.com" });
  // Hidden from people; bots fill it in, and the server drops those reports.
  const website = h("input", { name: "website", tabindex: -1, autocomplete: "off" });
  const status = h("p", { class: "report-status", role: "status" });
  const send = h("button", { type: "submit" }, "Send");
  const cancel = h("button", { type: "button", class: "alt", on: { click: () => dialog.close() } }, "Cancel");

  const form = h("form", { method: "dialog", on: { submit: (e) => { e.preventDefault(); submit(); } } },
    ...(listed ? [
      h("h2", {}, "Request support for your ESP32 board"),
      h("p", {}, "There are hundreds of ESP32 boards, each wired its own way. Tell us which one you'd like, and we'll look."),
    ] : boardFits(facts) ? [
      h("h2", {}, "Sorry, we don't know this board yet"),
      h("p", {}, "There are hundreds of ESP32 boards, each wired its own way, so we can't cover them all from the start."),
      h("p", {}, h("b", {}, "The good news: yours has everything Outrigger needs."), " We just need to set it up for its " +
        "screen and pins. Tell us which board it is, and we'll add it and send you a build to try."),
    ] : [
      h("h2", {}, "Sorry, this board can't run Outrigger yet"),
      h("p", {}, "There are hundreds of ESP32 boards, and Outrigger runs on only a few kinds so far. Tell us what this " +
        "one is, so we know which to support next."),
    ]),
    field("Which board is it?", board),
    field("Anything else we should know?", note),
    field("Your email (optional)", email, `So we can tell you when it's ready. Used for nothing else; we'll delete it if you ask (${FEEDBACK}).`),
    listed ? null : h("details", { class: "report-read" }, h("summary", {}, "What we'll send from your board"),
      h("dl", {}, read.flatMap(([k, v]) => [h("dt", {}, k), h("dd", {}, v)]))),
    h("div", { class: "report-trap", "aria-hidden": "true" }, field("Website", website)),
    status,
    h("div", { class: "row" }, send, cancel),
  );
  const dialog = h("dialog", { class: "report", "aria-label": "Ask us to add this board", on: { close: () => dialog.remove() } }, form);
  document.body.append(dialog);
  dialog.showModal();
  board.focus({ preventScroll: true }); // keep the heading and what-happens-next in view

  async function submit() {
    const report = buildReport({ facts, picked, verdict, installer, board: board.value, note: note.value,
      email: email.value, website: website.value });
    if (listed) report.psram_mb = null; // nothing was read: not "no PSRAM"
    const check = validateReport(report);
    status.classList.remove("report-sending");
    if (!check.ok) {
      status.textContent = check.problems.join(" ");
      return;
    }
    send.disabled = true;
    status.classList.add("report-sending");
    status.textContent = "Sending…";
    let res;
    let body = {};
    try {
      res = await fetch("/api/report", { method: "POST", headers: { "content-type": "application/json" },
        body: JSON.stringify(report) });
      body = await res.json().catch(() => ({}));
    } catch {
      res = null;
    }
    if (res && res.ok) {
      return form.replaceChildren(
        h("h2", {}, "Thanks!"),
        h("p", {}, report.email ? `We'll email ${report.email} when there's news.` : "Check back here for new boards."),
        h("div", { class: "row" }, h("button", { type: "button", on: { click: () => dialog.close() } }, "Close")));
    }
    send.disabled = false;
    status.classList.remove("report-sending");
    const why = res?.status === 429 ? "Too many tries from here. Try again in an hour"
      : body.problems?.length ? body.problems.join(" ") : `It didn't send (${res ? `error ${res.status}` : "no connection"}). Try again`;
    status.textContent = `${why.replace(/\.$/, "")}, or email ${FEEDBACK}.`;
  }
}
