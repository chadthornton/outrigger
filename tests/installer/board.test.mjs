// What the page reads off a board before installing (site/installer/board.js):
// the build tag, whether a backup store exists, and the sentence it shows.
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { readBuildTag, hasStore, describeBoard, buildName, classifyLayout, TAG_OFFSET } from "../../site/installer/board.js";

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

const hubDef = { app_set: "hub", commit: "def5678", dirty: false };
const loaderAbc = { app_set: "hub+L", commit: "abc1234", dirty: false };

test("describeBoard: a tagged build with a store, switching app sets", () => {
  const d = describeBoard({ tag: loaderAbc, layout: "ours", store: true }, hubDef);
  assert.equal(d.board, "This board runs Hub + Loader abc1234 and has a backup store, where the loader keeps FM-1 backups.");
  assert.equal(d.install, "Installing MIDI Hub def5678 replaces Hub + Loader. Your settings, Wi-Fi and backups stay.");
});

test("describeBoard: the same build again", () => {
  const d = describeBoard({ tag: hubDef, layout: "ours", store: false }, hubDef);
  assert.equal(d.board, "This board runs MIDI Hub def5678.");
  assert.equal(d.install, "It already runs this build. Installing it again changes nothing. Your settings and Wi-Fi stay.");
});

test("describeBoard: a newer commit of the same app set", () => {
  const d = describeBoard({ tag: { ...hubDef, commit: "1111111" }, layout: "ours", store: false }, hubDef);
  assert.equal(d.install, "Installing MIDI Hub def5678 updates it. Your settings and Wi-Fi stay.");
});

test("describeBoard: a dirty build shows its plus sign", () => {
  const d = describeBoard({ tag: { ...loaderAbc, dirty: true }, layout: "ours", store: false }, hubDef);
  assert.equal(d.board, "This board runs Hub + Loader abc1234+.");
});

test("describeBoard: our layout without a tag is an earlier build", () => {
  const d = describeBoard({ tag: null, layout: "ours", store: true }, hubDef);
  assert.equal(d.board, "This board runs an earlier usb-midi-host build and has a backup store, where the loader keeps FM-1 backups.");
  assert.equal(d.install, "Installing MIDI Hub def5678 replaces it. Your settings, Wi-Fi and backups stay.");
});

test("describeBoard: other firmware", () => {
  const d = describeBoard({ tag: null, layout: "other", store: false }, hubDef);
  assert.equal(d.board, "This board runs other firmware.");
  assert.equal(d.install, "Installing MIDI Hub def5678 replaces it. Settings from that firmware may not carry over.");
});

test("describeBoard: a blank board", () => {
  const d = describeBoard({ tag: null, layout: "blank", store: false }, hubDef);
  assert.equal(d.board, "This board is empty.");
  assert.equal(d.install, "Installing puts MIDI Hub def5678 on it.");
});

test("classifyLayout: blank, ours (has the store and recovery slots), or other", () => {
  assert.equal(classifyLayout({}), "blank");
  assert.equal(classifyLayout({ nvs: [0x9000, 0x5000], app0: [0x10000, 0x300000], recovery: [0x310000, 0xc0000], store: [0x3d0000, 0xc20000] }), "ours");
  assert.equal(classifyLayout({ nvs: [0x9000, 0x5000], app0: [0x10000, 0x140000], spiffs: [0x290000, 0x160000] }), "other");
});
