// What the page reads off a board before installing, and how it says it.
// Pure: the bytes come from esptool-js reads in install.js, so `make test`
// runs this on the Mac (tests/installer/board.test.mjs).

// The build tag (lib/platform/src/build_tag.h) sits right after the 256-byte
// esp_app_desc_t, which sits after the image and segment headers: app image
// offset 0x120, flash 0x10120. tests/installer/tags.json holds its cases.
export const TAG_OFFSET = 0x120;
export const TAG_LENGTH = 20;
const TAG_MAGIC = 0x54484d55; // "UMHT"
const TAG_LAYOUT = 1;

// bytes: the app's first TAG_OFFSET + TAG_LENGTH bytes or more.
// Returns {commit, dirty, app_set}, or null for no tag (a build from before
// tags, other firmware) or a layout this page doesn't know.
export function readBuildTag(bytes) {
  if (bytes.length < TAG_OFFSET + TAG_LENGTH) return null;
  const view = new DataView(bytes.buffer, bytes.byteOffset + TAG_OFFSET, TAG_LENGTH);
  if (view.getUint32(0, true) !== TAG_MAGIC || view.getUint16(4, true) !== TAG_LAYOUT) return null;
  const label = bytes.subarray(TAG_OFFSET + 12, TAG_OFFSET + 20);
  const end = label.indexOf(0);
  return {
    commit: view.getUint32(8, true).toString(16).padStart(7, "0"),
    dirty: view.getUint8(6) !== 0,
    app_set: new TextDecoder().decode(end < 0 ? label : label.subarray(0, end)),
  };
}

// The first 16 bytes of the store's blocks 0 and 1: littlefs keeps its
// superblock in that pair, with "littlefs" at byte 8 of whichever is current.
export function hasStore(block0, block1) {
  const magic = new TextEncoder().encode("littlefs");
  return [block0, block1].some((b) => b.length >= 16 && magic.every((c, i) => b[8 + i] === c));
}

// The board's partition table (plan.js parseTable): empty, this project's
// layout (a recovery slot and a store), or someone else's.
export function classifyLayout(table) {
  if (!Object.keys(table).length) return "blank";
  return table.store && table.recovery && table.app0 && table.app0[0] === 0x10000 ? "ours" : "other";
}

const NAMES = {
  hub: "MIDI Hub",
  "hub+L": "Hub + Loader",
  all: "Everything",
  "hub+LW": "Hub + Loader with the Wi-Fi page",
  "hub+W": "MIDI Hub with the Wi-Fi page",
};

// app set label (apps.h kAppSetLabel) -> the installer's words
export function buildName(label) {
  return NAMES[label] || "a custom build";
}

const version = (tag) => `${tag.commit}${tag.dirty ? "+" : ""}`;

// board: {tag, layout, store}; build: the tag of the build about to be
// installed (manifest.json "build"). Returns two sentences: what the board
// holds, and what installing does to it.
export function describeBoard(board, build) {
  const next = `${buildName(build.app_set)} ${version(build)}`;
  const storeClause = board.store ? " and has a backup store, where the loader keeps FM-1 backups" : "";
  const keeps = board.store ? "Your settings, Wi-Fi and backups stay." : "Your settings and Wi-Fi stay.";

  if (board.layout === "blank") return { board: "This board is empty.", install: `Installing puts ${next} on it.` };
  if (board.layout === "other") {
    return { board: "This board runs other firmware.", install: `Installing ${next} replaces it. Settings from that firmware may not carry over.` };
  }
  if (!board.tag) {
    return { board: `This board runs an earlier usb-midi-host build${storeClause}.`, install: `Installing ${next} replaces it. ${keeps}` };
  }
  const now = buildName(board.tag.app_set);
  const said = `This board runs ${now} ${version(board.tag)}${storeClause}.`;
  if (board.tag.app_set !== build.app_set) return { board: said, install: `Installing ${next} replaces ${now}. ${keeps}` };
  if (version(board.tag) === version(build)) {
    // Two dirty builds of one commit can differ, so only a clean one is "this build".
    const same = build.dirty ? "It already runs this commit. Installing it again rewrites the program."
      : "It already runs this build. Installing it again changes nothing.";
    return { board: said, install: `${same} ${keeps}` };
  }
  return { board: said, install: `Installing ${next} updates it. ${keeps}` };
}
