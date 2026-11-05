// A pinned deploy's check (tools/check_pinned.mjs): the staged site's own
// installer code loads every build of the firmware it is pinned to.
import { test } from "node:test";
import assert from "node:assert/strict";
import { cpSync, mkdtempSync, mkdirSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { createHash } from "node:crypto";
import { pinnedProblems } from "../../tools/check_pinned.mjs";

const root = new URL("../../", import.meta.url);
const sha = (data) => createHash("sha256").update(data).digest("hex");
const named = (manifest) => [{ label: "Check my ESP32", manifest, board: "B1" }, { label: "Check my ESP32", manifest: null }];
const page = (builds) => `<div class="install" data-builds="${JSON.stringify(builds).replaceAll('"', "&quot;")}"></div>`;

// A staged site: this commit's installer code, one board with a loader build.
function site() {
  const dir = mkdtempSync(join(tmpdir(), "pinned-"));
  cpSync(new URL("site/installer", root), join(dir, "installer"), { recursive: true });
  const build = join(dir, "firmware", "b1", "hub-loader");
  mkdirSync(build, { recursive: true });
  writeFileSync(join(build, "app.bin"), "app");
  writeFileSync(join(build, "store-template.json"), "{}");
  writeFileSync(join(build, "manifest.json"), JSON.stringify({
    build: { commit: "abc1234", app_set: "hub+LW" },
    builds: [{ parts: [{ path: "app.bin", offset: 65536, sha256: sha("app") }] }],
    store_template: "store-template.json", store_template_sha256: sha("{}"),
  }));
  writeFileSync(join(dir, "index.html"), page(named("firmware/b1/hub-loader/manifest.json")));
  return dir;
}

async function problems(change = () => {}) {
  const dir = site();
  try {
    change(dir);
    return await pinnedProblems(dir);
  } finally {
    rmSync(dir, { recursive: true });
  }
}

test("a site its installer can install from passes", async () => assert.deepEqual(await problems(), []));

test("a part that doesn't match its manifest fails", async () => {
  const found = await problems((dir) => writeFileSync(join(dir, "firmware/b1/hub-loader/app.bin"), "other"));
  assert.match(found.join("\n"), /b1\/hub-loader.*intact/);
});

test("a store template that doesn't match its manifest fails", async () => {
  const found = await problems((dir) => writeFileSync(join(dir, "firmware/b1/hub-loader/store-template.json"), "[]"));
  assert.match(found.join("\n"), /store template/);
});

test("a manifest the page names but the firmware lacks fails", async () => {
  const found = await problems((dir) => writeFileSync(join(dir, "index.html"), page(named("firmware/b2/hub-loader/manifest.json"))));
  assert.match(found.join("\n"), /index\.html names firmware\/b2\/hub-loader\/manifest\.json/);
});

test("a board folder no page names fails", async () => {
  const found = await problems((dir) => {
    cpSync(join(dir, "firmware/b1"), join(dir, "firmware/b3"), { recursive: true });
  });
  assert.match(found.join("\n"), /firmware\/b3 is named by no page/);
});

test("a manifest the installer can't read fails", async () => {
  const found = await problems((dir) => writeFileSync(join(dir, "firmware/b1/hub-loader/manifest.json"), '{"builds": []}'));
  assert.match(found.join("\n"), /b1\/hub-loader/);
});

test("no firmware fails", async () => {
  const found = await problems((dir) => rmSync(join(dir, "firmware"), { recursive: true }));
  assert.match(found.join("\n"), /no firmware/);
});
