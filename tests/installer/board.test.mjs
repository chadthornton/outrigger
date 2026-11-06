// What the page reads off a board before installing (site/installer/board.js):
// the build tag, whether a backup store exists, and the sentence it shows.
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { readBuildTag, hasStore, describeBoard, compareBuilds, buildName, classifyLayout, TAG_OFFSET } from "../../site/installer/board.js";

const root = new URL("../../", import.meta.url);
const tags = JSON.parse(readFileSync(new URL("tests/installer/tags.json", root)));
const fromHex = (h) => Uint8Array.from(h.match(/../g) || [], (b) => parseInt(b, 16));

test("the tag sits right after esp_app_desc_t", () => assert.equal(TAG_OFFSET, tags.offset));

for (const c of tags.cases) {
  test(`readBuildTag: ${c.name}`, () => {
    // The page reads the app's first bytes; put the tag where it sits.
    const head = new Uint8Array(tags.offset + 20).fill(0xff);
    const tag = fromHex(c.hex);
    head.set(tag, tags.offset);
    const bytes = tag.length < 20 ? head.subarray(0, tags.offset + tag.length) : head;
    assert.deepEqual(readBuildTag(bytes), c.expect);
  });
}

// The first 16 bytes of the store's blocks 0 and 1, as littlefs writes them
// (checked against littlefs-python, disk version 2.0).
const SUPER = fromHex("00000000f00ffff76c6974746c656673");
const ERASED = new Uint8Array(16).fill(0xff);

test("hasStore: a littlefs superblock in either block of the pair", () => {
  assert.equal(hasStore(SUPER, ERASED), true);
  assert.equal(hasStore(ERASED, SUPER), true);
  assert.equal(hasStore(ERASED, ERASED), false);
  assert.equal(hasStore(new Uint8Array(16), new Uint8Array(16)), false);
});

test("buildName: the installer's words for each app set label", () => {
  assert.equal(buildName("hub"), "MIDI Hub");
  assert.equal(buildName("hub+L"), "Hub + Loader");
  assert.equal(buildName("all"), "Everything");
  assert.equal(buildName("hub+LW"), "Hub + Loader with the Wi-Fi page");
  assert.equal(buildName("something new"), "a custom build");
});

const boardNames = { 0: 'Waveshare 1.69"', 1: 'Waveshare 3.5"' };
const hubDef = { app_set: "hub", commit: "def5678", dirty: false };
const loaderAbc = { app_set: "hub+L", commit: "abc1234", dirty: false };

test("describeBoard: a tagged build with a store, switching app sets", () => {
  const d = describeBoard({ tag: loaderAbc, layout: "ours", store: true }, hubDef, {}, ["abc1234"]);
  assert.equal(d.board, "This board runs Hub + Loader abc1234.");
  assert.equal(d.install, "Installing MIDI Hub def5678 replaces Hub + Loader. Your settings, Wi-Fi and backups stay.");
});

test("describeBoard: the same build again", () => {
  const d = describeBoard({ tag: hubDef, layout: "ours", store: false }, hubDef);
  assert.equal(d.board, "This board runs MIDI Hub def5678.");
  assert.equal(d.install, "It already runs this build. Installing it again changes nothing. Your settings and Wi-Fi stay.");
});

test("describeBoard: an older commit of the same app set updates", () => {
  const d = describeBoard({ tag: { ...hubDef, commit: "1111111" }, layout: "ours", store: false }, hubDef, {}, ["2222222", "1111111"]);
  assert.equal(d.install, "Installing MIDI Hub def5678 updates it. Your settings and Wi-Fi stay.");
  assert.equal(d.newer, undefined);
});

// Tonight's bug (2026-11-05): a board on a91874d was offered e49a1aa, an ancestor, as "updates it".
const e49 = { app_set: "hub+LW", commit: "e49a1aa", dirty: false };
const onA91 = { tag: { app_set: "hub+LW", commit: "a91874d", dirty: false }, layout: "ours", store: true };

test("describeBoard: a board on a newer build says so, and isn't offered an update", () => {
  const d = describeBoard(onA91, e49, {}, ["0f940f7", "6f95342"]);
  assert.equal(d.newer, true);
  assert.equal(d.board, "This board runs Hub + Loader with the Wi-Fi page a91874d, a newer build than this page installs.");
  assert.equal(d.install, "Installing puts the older Hub + Loader with the Wi-Fi page e49a1aa on it. Your settings, Wi-Fi and backups stay.");
});

test("describeBoard: newer wins over a different app set and a wrong-board pick", () => {
  const d = describeBoard({ ...onA91, tag: { ...onA91.tag, board: 1 } }, { ...hubDef, board: 0 }, boardNames, []);
  assert.equal(d.newer, true);
  assert.match(d.install, /^Installing puts the older MIDI Hub def5678 on it\./);
  assert.match(d.warning, /^This board runs the build for the Waveshare 3\.5"/);
});

test("describeBoard: without the build's history it can't tell, so it neither updates nor warns", () => {
  const d = describeBoard(onA91, e49);
  assert.equal(d.newer, undefined);
  assert.equal(d.install, "Installing Hub + Loader with the Wi-Fi page e49a1aa replaces it. Your settings, Wi-Fi and backups stay.");
});

test("compareBuilds: same, older (in the history), newer (not in it), unknown (no history)", () => {
  const tag = (commit, dirty = false) => ({ app_set: "hub+L", commit, dirty });
  assert.equal(compareBuilds(tag("abc1234"), tag("abc1234"), []), "same");
  assert.equal(compareBuilds(tag("abc1234", true), tag("abc1234"), []), "older");
  assert.equal(compareBuilds(tag("1111111"), tag("abc1234"), ["1111111"]), "older");
  assert.equal(compareBuilds(tag("9999999"), tag("abc1234"), ["1111111"]), "newer");
  assert.equal(compareBuilds(tag("9999999"), tag("abc1234"), undefined), "unknown");
  assert.equal(compareBuilds(tag("abc1234"), tag("abc1234")), "same");
});

test("describeBoard: a dirty build shows its plus sign", () => {
  const d = describeBoard({ tag: { ...loaderAbc, dirty: true }, layout: "ours", store: false }, hubDef);
  assert.equal(d.board, "This board runs Hub + Loader abc1234+.");
});

test("describeBoard: our layout without a tag is an earlier build", () => {
  const d = describeBoard({ tag: null, layout: "ours", store: true }, hubDef);
  assert.equal(d.board, "This board runs an earlier Outrigger build.");
  assert.equal(d.install, "Installing MIDI Hub def5678 updates it. Your settings, Wi-Fi and backups stay.");
});

test("describeBoard: other firmware", () => {
  const d = describeBoard({ tag: null, layout: "other", store: false }, hubDef);
  assert.equal(d.board, "This board runs other firmware.");
  assert.equal(d.install, "Installing MIDI Hub def5678 replaces it. Settings from that firmware may not carry over.");
});

test("describeBoard: a blank board", () => {
  const d = describeBoard({ tag: null, layout: "blank", store: false }, hubDef);
  assert.equal(d.board, "Nothing on it yet: ready for Outrigger.");
  assert.equal(d.install, "Installing puts MIDI Hub def5678 on it.");
});

test("classifyLayout: blank, ours (has the store and recovery slots), or other", () => {
  assert.equal(classifyLayout({}), "blank");
  assert.equal(classifyLayout({ nvs: [0x9000, 0x5000], app0: [0x10000, 0x300000], recovery: [0x310000, 0xc0000], store: [0x3d0000, 0xc20000] }), "ours");
  assert.equal(classifyLayout({ nvs: [0x9000, 0x5000], app0: [0x10000, 0x140000], spiffs: [0x290000, 0x160000] }), "other");
});

test("describeBoard: a dirty build of the same commit can't promise nothing changes", () => {
  const dirty = { ...hubDef, dirty: true };
  const d = describeBoard({ tag: dirty, layout: "ours", store: false }, dirty);
  assert.equal(d.install, "It already runs this commit. Installing it again rewrites the program. Your settings and Wi-Fi stay.");
});

test("describeBoard: a build for another board than the one picked warns", () => {
  const d = describeBoard({ tag: { ...loaderAbc, board: 1 }, layout: "ours", store: true }, { ...hubDef, board: 0 }, boardNames);
  assert.equal(d.warning, 'This board runs the build for the Waveshare 3.5", and you picked the Waveshare 1.69". ' +
    "Pick the board you have: the wrong build leaves its screen dark or garbled. Installing again with the right pick fixes that.");
  assert.equal(d.board, "This board runs Hub + Loader abc1234.");
});

test("describeBoard: the same board, or builds from before the board byte, don't warn", () => {
  assert.equal(describeBoard({ tag: { ...loaderAbc, board: 1 }, layout: "ours", store: false }, { ...hubDef, board: 1 }, boardNames).warning, undefined);
  assert.equal(describeBoard({ tag: loaderAbc, layout: "ours", store: false }, { ...hubDef, board: 0 }, boardNames).warning, undefined);
  assert.equal(describeBoard({ tag: null, layout: "blank", store: false }, { ...hubDef, board: 1 }, boardNames).warning, undefined);
});

test("describeBoard: a board this page has no name for", () => {
  const d = describeBoard({ tag: { ...loaderAbc, board: 7 }, layout: "ours", store: false }, { ...hubDef, board: 0 }, boardNames);
  assert.match(d.warning, /^This board runs the build for another board, and you picked the Waveshare 1\.69"\./);
});
