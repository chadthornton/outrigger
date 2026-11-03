// The "Report this board" pop-up: opens from an install area (area.js) when
// the chip check finds no profile for a board, or refuses its chip. It shows
// what the installer read, takes the board's name, a note and an optional
// email, and posts them to /api/report (functions/api/report.js). What goes
// out is built by report.js, from a fixed list of fields: never the MAC.
import { buildReport, validateReport, boardFits, LIMITS } from "./report.js";
import { flashBytesFromId, psramMbFromFeatures, USB_BRIDGES } from "./chip.js";
import { h } from "./ui.js";

const FEEDBACK = "feedback@outrigger.music";

function usbWords(vid) {
  if (vid == null) return "didn't say";
  const hex = `0x${vid.toString(16).padStart(4, "0")}`;
  if (vid === 0x303a) return `the chip's own USB (${hex})`;
  return USB_BRIDGES[vid] ? `${USB_BRIDGES[vid]} (${hex})` : hex;
}

// {facts, picked, verdict, installer}: what the area read and decided.
export function openReportForm({ facts, picked, verdict, installer }) {
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
    ...(boardFits(facts) ? [
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
    h("details", { class: "report-read" }, h("summary", {}, "What we'll send from your board"),
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
