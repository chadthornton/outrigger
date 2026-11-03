// The "Report this board" pop-up: opens from an install area (area.js) when
// the chip check finds no profile for a board, or refuses its chip. It shows
// what the installer read, takes the board's name, a note and an optional
// email, and posts them to /api/report (functions/api/report.js). What goes
// out is built by report.js, from a fixed list of fields: never the MAC.
import { buildReport, validateReport, LIMITS } from "./report.js";
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
    ["You picked", picked],
  ];
  const field = (label, input, help) => h("label", { class: "report-field" }, h("span", {}, label), input,
    help ? h("small", {}, help) : null);
  const board = h("input", { name: "board", required: true, maxlength: LIMITS.board, autocomplete: "off",
    placeholder: "e.g. Freenove ESP32-S3 2.8\", or a link to where you bought it" });
  const note = h("textarea", { name: "note", rows: 4, maxlength: LIMITS.note,
    placeholder: "What's on the board, what you'd use it for, anything odd you noticed." });
  const email = h("input", { name: "email", type: "email", maxlength: LIMITS.email, autocomplete: "email",
    placeholder: "you@example.com" });
  // Hidden from people; bots fill it in, and the server drops those reports.
  const website = h("input", { name: "website", tabindex: -1, autocomplete: "off" });
  const status = h("p", { class: "report-status", role: "status" });
  const send = h("button", { type: "submit" }, "Send report");
  const cancel = h("button", { type: "button", class: "alt", on: { click: () => dialog.close() } }, "Cancel");

  const form = h("form", { method: "dialog", on: { submit: (e) => { e.preventDefault(); submit(); } } },
    h("h2", {}, "Tell us about this board"),
    h("p", {}, "We read every report ourselves. If your board can run Outrigger, we look up its pins in the maker's " +
      "schematic, write a profile for it, and send you a build to try. If it can't, " +
      "we'll say why, and what would work instead. Reports also tell us which boards to add first."),
    h("div", { class: "report-read" }, h("p", {}, h("b", {}, "What the installer read")),
      h("dl", {}, read.flatMap(([k, v]) => [h("dt", {}, k), h("dd", {}, v)])),
      h("p", { class: "report-small" }, "That's all it sends from your board. Not its serial number or network address.")),
    field("Board name or link", board),
    field("Anything else? (optional)", note),
    field("Email (optional)", email, "Leave it and we'll keep you in the loop on this board: when there's a build to " +
      `try, and when it's ready. We use it only for that, and delete it whenever you ask at ${FEEDBACK}.`),
    h("div", { class: "report-trap", "aria-hidden": "true" }, field("Website", website)),
    status,
    h("div", { class: "row" }, send, cancel),
  );
  const dialog = h("dialog", { class: "report", "aria-label": "Report this board", on: { close: () => dialog.remove() } }, form);
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
        h("h2", {}, "Thanks, we got it"),
        h("p", {}, report.email
          ? `We'll write to ${report.email} when we know more about the ${report.board}.`
          : "We'll look at it soon. Without an email we can't write back, so watch this page for new boards."),
        h("div", { class: "row" }, h("button", { type: "button", on: { click: () => dialog.close() } }, "Close")));
    }
    send.disabled = false;
    status.classList.remove("report-sending");
    const why = res?.status === 429 ? "That's a lot of reports from here in one hour. Try again later"
      : body.problems?.length ? body.problems.join(" ") : `It didn't send (${res ? `error ${res.status}` : "no connection"}). Try again`;
    status.textContent = `${why.replace(/\.$/, "")}, or email ${FEEDBACK}.`;
  }
}
