// Can this ESP32 run Outrigger? (site/installer/chip.js), against the cases in
// chips.json: what esptool-js reports for boards people may plug in.
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { checkChip, chipFamily, flashBytesFromId, psramMbFromFeatures } from "../../site/installer/chip.js";

const { cases } = JSON.parse(readFileSync(new URL("chips.json", import.meta.url)));
// The site's boards, as docs/site-redesign/build.py passes them (firmware/boards.json).
const boards = JSON.parse(readFileSync(new URL("../../firmware/boards.json", import.meta.url))).boards;
const profiles = Object.values(boards).map((b) => ({ board: b.short_name, tag_id: b.tag_id, flash_mb: b.flash_mb, psram_mb: b.psram_mb, tested: b.tested }));

for (const c of cases) {
  test(`checkChip: ${c.name}`, () => {
    const picked = c.picked === null ? null : profiles.find((p) => p.board === c.picked);
    assert.ok(c.picked === null || picked, `no profile named ${c.picked}`);
    const got = checkChip(c.facts, picked, profiles);
    assert.equal(got.ok, c.expect.ok, got.words);
    for (const w of c.expect.words) assert.ok(got.words.includes(w), `wanted ${JSON.stringify(w)} in: ${got.words}`);
  });
}

test("chipFamily: esptool-js's descriptions", () => {
  assert.equal(chipFamily("ESP32-S3 (QFN56) (revision v0.2)"), "ESP32-S3");
  assert.equal(chipFamily("ESP32-D0WD-V3 (revision v3.1)"), "ESP32");
  assert.equal(chipFamily("ESP32-C61 (revision v1.0)"), "ESP32-C61");
  assert.equal(chipFamily("ESP32-S2FH4 (revision v0.0)"), "ESP32-S2");
  assert.equal(chipFamily(""), null);
});

test("flashBytesFromId: capacity byte, with makers' offsets", () => {
  assert.equal(flashBytesFromId(0x184018), 16 * 1048576); // Winbond W25Q128
  assert.equal(flashBytesFromId(0x174017), 8 * 1048576);
  assert.equal(flashBytesFromId(0x384000), 16 * 1048576); // capacity byte 0x38: 2^(0x38 - 0x20)
  assert.equal(flashBytesFromId(0xffffff), null);
  assert.equal(flashBytesFromId(0), null);
});

test("psramMbFromFeatures", () => {
  assert.equal(psramMbFromFeatures(["Wi-Fi", "Embedded PSRAM 8MB (AP_3v3)"]), 8);
  assert.equal(psramMbFromFeatures(["Wi-Fi"]), 0);
});
