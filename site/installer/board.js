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
// Returns {commit, dirty, app_set, board}, or null for no tag (a build from
// before tags, other firmware) or a layout this page doesn't know. board is the
// profile's tag_id (firmware/boards.json): 0, the 1.69, in builds from before it.
export function readBuildTag(bytes) {
  if (bytes.length < TAG_OFFSET + TAG_LENGTH) return null;
  const view = new DataView(bytes.buffer, bytes.byteOffset + TAG_OFFSET, TAG_LENGTH);
  if (view.getUint32(0, true) !== TAG_MAGIC || view.getUint16(4, true) !== TAG_LAYOUT) return null;
  const label = bytes.subarray(TAG_OFFSET + 12, TAG_OFFSET + 20);
  const end = label.indexOf(0);
  return {
    commit: view.getUint32(8, true).toString(16).padStart(7, "0"),
    dirty: view.getUint8(6) !== 0,
    board: view.getUint8(7),
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

// How the board's build stands against the one about to be installed: "same",
// "older" (the board's commit is in ancestors, the offered build's history),
// "newer" (it isn't: a later commit or a branch this page doesn't hold), or
// "unknown" (no history: a manifest from before it was recorded). The tag holds
// only the short commit, so ancestors are 7-character prefixes (build_site.py).
export function compareBuilds(tag, build, ancestors) {
  if (version(tag) === version(build)) return "same";
  if (tag.commit === build.commit) return "older"; // a dirty build of this commit
  if (!Array.isArray(ancestors)) return "unknown";
  // git's --short=7 is a minimum: a colliding prefix gives the tag 8 characters.
  return ancestors.includes(tag.commit.slice(0, 7)) ? "older" : "newer";
}

// board: {tag, layout, store}; build: the tag of the build about to be
// installed (manifest.json "build"); names: tag_id -> the boards' short names;
// ancestors: manifest.json "ancestors". Returns two lines: what the board holds
// (board), and what installing does to it (install); newer when the board runs
// a later build than this one, so Install isn't offered first; and a warning
// when it runs the build for another board than the one picked. The chip
// can't say which board it sits on, so the pick is the user's.
export function describeBoard(board, build, names = {}, ancestors) {
  const words = describeBuild(board, build, ancestors);
  const theirs = board.tag ? board.tag.board ?? 0 : null;
  const picked = build.board ?? 0;
  if (theirs === null || theirs === picked) return words;
  const name = (id) => (names[id] ? `the ${names[id]}` : "another board");
  return {
    ...words,
    install: words.newer ? words.install : `Installing ${buildName(build.app_set)} ${version(build)} replaces it. ${keepsFor(board)}`,
    warning: `This board runs the build for ${name(theirs)}, and you picked ${name(picked)}. ` +
      "Pick the board you have: the wrong build leaves its screen dark or garbled. Installing again with the right pick fixes that.",
  };
}

const keepsFor = (board) => (board.store ? "Your settings, Wi-Fi and backups stay." : "Your settings and Wi-Fi stay.");

function describeBuild(board, build, ancestors) {
  const next = `${buildName(build.app_set)} ${version(build)}`;
  const keeps = keepsFor(board);

  if (board.layout === "blank") return { board: "Nothing on it yet: ready for Outrigger.", install: `Installing puts ${next} on it.` };
  if (board.layout === "other") {
    return { board: "This board runs other firmware.", install: `Installing ${next} replaces it. Settings from that firmware may not carry over.` };
  }
  // Our layout without a tag: a build from before tags, so older than any this page offers.
  if (!board.tag) return { board: "This board runs an earlier Outrigger build.", install: `Installing ${next} updates it. ${keeps}` };
  const now = buildName(board.tag.app_set);
  const said = `This board runs ${now} ${version(board.tag)}.`;
  const relation = compareBuilds(board.tag, build, ancestors);
  if (relation === "newer") {
    return { board: `This board runs ${now} ${version(board.tag)}, a newer build than this page installs.`, newer: true,
      install: `Installing puts the older ${board.tag.app_set === build.app_set ? version(build) : next} on it. ${keeps}` };
  }
  if (board.tag.app_set !== build.app_set) return { board: said, install: `Installing ${next} replaces ${now}. ${keeps}` };
  if (relation === "same") {
    // Two dirty builds of one commit can differ, so only a clean one is "this build".
    const same = build.dirty ? "It already runs this commit. Installing it again rewrites the program."
      : "It already runs this build. Installing it again changes nothing.";
    return { board: said, install: `${same} ${keeps}` };
  }
  return { board: said, install: `Installing ${next} ${relation === "older" ? "updates" : "replaces"} it. ${keeps}` };
}
