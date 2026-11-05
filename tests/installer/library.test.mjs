// The firmware library page's logic (site/installer/library.js): catalog and
// store to rows, search, favorites, the cart against the board's limits.
import { test } from "node:test";
import assert from "node:assert/strict";
import { addableFirmwares } from "../../site/installer/add.js";
import { BACKUP_RESERVE, PACKAGE_ROOM, applyOutcome, addError, boardId, canAdd, countLine, libraryRows, meter, room,
  shownRows, storageLine, toggle } from "../../site/installer/library.js";

const catalog = {
  stock: { pkg: "fw/mvave/FM-1_015.fwsc", sha256: "aa", size: 699956, product: "FM-1_015" },
  firmwares: [
    { id: "felucca", name: "Felucca", version: "1.8.2", pkg: "f.fwsc", sha256: "f2", size: 1, lic: "GPL-3.0", installs: 9, author: "hugelton", what: "Groovebox with song mode." },
    { id: "skiff", name: "Skiff", version: "2.2", pkg: "s.fwsc", sha256: "s2", size: 1, lic: "GPL-3.0", installs: 8, author: "skiff team", what: "Live performance." },
    { id: "skiff-smk", name: "Skiff (smk)", version: "2.2", pkg: "k.fwsc", sha256: "k2", size: 1, lic: "GPL-3.0", installs: 1 },
    { id: "x0x", name: "X0X", version: "1.2.0", pkg: "x.fwsc", sha256: "x1", size: 1, lic: "MIT", installs: 7, author: "charlesvestal", what: "909, 808 and two 303s." },
    { id: "etch", name: "ETCH", version: "1.88", pkg: "e.fwsc", sha256: "e1", size: 1, lic: "MIT", installs: 6, author: "someone", what: "Drum-focused." },
    { id: "sloop", name: "SLOOP", version: "2.8.4", pkg: "l.fwsc", sha256: "l1", size: 1, lic: "GPL-3.0", installs: 5 },
    { id: "jangada", name: "Jangada", version: "1", pkg: "j.fwsc", sha256: "j1", size: 1, lic: "GPL-3.0", installs: 4 },
    { id: "emb", name: "emb", version: "1", pkg: "m.fwsc", sha256: "m1", size: 1, lic: null, installs: 3 },
  ],
};
const lists = addableFirmwares(catalog);
const stored = [
  { id: "stock", sha256: "aa" },
  { id: "felucca-1.8.0", sha256: "f0" },  // an older Felucca: update to 1.8.2
  { id: "skiff-2.2", sha256: "s2" },      // current
  { id: "my-own-thing", sha256: "zz" },   // added from the Mac, not in the catalog
];

test("boardId: as receive.cpp names packages", () => {
  assert.equal(boardId("felucca", "1.8.2"), "felucca-1.8.2");
  assert.equal(boardId("stock", ""), "stock");
  assert.equal(boardId("tessera-fm-1-83773022", "83773022"), "tessera-fm-1-83773022");
});

test("libraryRows: stored packages matched by board id, never by splitting it", () => {
  const { board, available, kept } = libraryRows(lists, catalog, stored);
  assert.deepEqual(kept.map((r) => r.name), ["M-VAVE stock"]);
  const by = Object.fromEntries(board.map((r) => [r.key, r]));
  assert.equal(by["felucca-1.8.0"].state, "update");
  assert.equal(by["felucca-1.8.0"].version, "1.8.0");
  assert.equal(by["felucca-1.8.0"].fw.version, "1.8.2");
  assert.equal(by["skiff-2.2"].state, "current");
  assert.equal(by["skiff-2.2"].version, "2.2");
  assert.equal(by["skiff-2.2"].author, "skiff team");
  assert.equal(by["my-own-thing"].state, "unknown");
  assert.equal(by["my-own-thing"].name, "my-own-thing");
  // skiff-2.2 belongs to skiff, not skiff-smk, so skiff-smk is still available; stock never is.
  assert.deepEqual(available.map((r) => r.key), ["skiff-smk", "x0x", "etch"].sort((a, b) =>
    lists.addable.findIndex((f) => f.project === a) - lists.addable.findIndex((f) => f.project === b)));
  assert.ok(!available.some((r) => r.key === "stock"));
});

test("libraryRows: one update per project, none when the latest is already stored", () => {
  const two = [{ id: "felucca-1.7", sha256: "a" }, { id: "felucca-1.8.0", sha256: "b" }];
  assert.equal(libraryRows(lists, catalog, two).board.filter((r) => r.state === "update").length, 1);
  const latest = [{ id: "felucca-1.8.0", sha256: "b" }, { id: "felucca-1.8.2", sha256: "f2" }];
  assert.deepEqual(libraryRows(lists, catalog, latest).board.map((r) => r.state), ["old", "current"]);
});

test("libraryRows: no board, everything addable is available", () => {
  const { board, available } = libraryRows(lists, catalog, null);
  assert.equal(board.length, 0);
  assert.deepEqual(available.map((r) => r.key).sort(), ["etch", "felucca", "skiff", "skiff-smk", "x0x"]);
});

test("libraryRows: riskier firmwares are coming soon while held, Add… once add.js marks them; other held stay held", () => {
  // By hand, so the test holds whichever add.js is on master (the riskier-firmwares change moves SLOOP to addable).
  const why = { old: "Not yet: it's built on SLOOP or an older Felucca, and can write parts of the FM-1 Outrigger can't clear or restore.",
    bt: "Not yet: it's built on Melodee, which writes the FM-1's Bluetooth sector Outrigger uses to tell FM-1s apart.",
    lic: "Not from the list: it has no license, so add it yourself once files of your own are allowed." };
  const x0x = { project: "x0x", name: "X0X", version: "1.2.0" };
  const before = { addable: [x0x], held: [{ project: "sloop", name: "SLOOP", why: why.old },
    { project: "jangada", name: "Jangada", why: why.old }, { project: "orange-cat", name: "Orange Cat", why: why.bt },
    { project: "emb", name: "emb", why: why.lic }] };
  const by = Object.fromEntries(libraryRows(before, catalog, null).riskier.map((r) => [r.key, r]));
  assert.equal(by.sloop.state, "soon");
  assert.equal(by.sloop.version, "2.8.4");  // from the catalog: held entries carry none
  assert.equal(by.jangada.state, "held");
  assert.equal(by["orange-cat"].state, "held");
  assert.equal(by.emb, undefined);  // held for its license, not for what it writes
  const after = libraryRows({ addable: [x0x, { project: "sloop", name: "SLOOP", version: "2.8.4", riskier: true }],
    held: before.held.filter((f) => f.project !== "sloop") }, catalog, null);
  assert.equal(after.riskier.find((r) => r.key === "sloop").state, "riskier");
  assert.deepEqual(after.available.map((r) => r.key), ["x0x"]);
});

test("shownRows: search on name, author and description; favorites first", () => {
  const { available } = libraryRows(lists, catalog, null);
  assert.deepEqual(shownRows(available, "303").map((r) => r.key), ["x0x"]);
  assert.deepEqual(shownRows(available, "  CHARLES ").map((r) => r.key), ["x0x"]);
  assert.deepEqual(shownRows(available, "drum").map((r) => r.key), ["etch"]);
  const ordered = shownRows(available, "", ["etch", "x0x"]).map((r) => r.key);
  assert.deepEqual(ordered.slice(0, 2), ["x0x", "etch"]);  // favorites keep the list's own order
  assert.equal(ordered.length, available.length);
  assert.equal(countLine(1, 5, "303"), "Showing 1 of 5");
  assert.equal(countLine(5, 5, " "), "5 firmwares");
  assert.deepEqual(toggle(toggle(["a"], "b"), "a"), ["b"]);
});

test("the board's limits as receive.h has them", () => {
  assert.equal(BACKUP_RESERVE, 272 * 4096);
  assert.equal(PACKAGE_ROOM, 147 * 4096);
});

test("room: the count limit and the bytes each add needs, whichever is less", () => {
  const plenty = { packages: 4, maxPackages: 10, free: 12 * 1048576 };
  assert.equal(room(plenty), 6);
  const tight = { packages: 4, maxPackages: 10, free: BACKUP_RESERVE + 2 * PACKAGE_ROOM + 100 };
  assert.equal(room(tight), 2);
  assert.equal(room({ packages: 4, maxPackages: 10, free: BACKUP_RESERVE + PACKAGE_ROOM - 1 }), 0);
  assert.equal(room({ packages: 10, maxPackages: 10, free: 12 * 1048576 }), 0);
  assert.equal(canAdd(tight, ["a"]), true);
  assert.equal(canAdd(tight, ["a", "b"]), false);
});

test("meter: stock, stored, cart, free, and slots memory can't fill", () => {
  const tight = { packages: 4, maxPackages: 10, free: BACKUP_RESERVE + 3 * PACKAGE_ROOM };
  assert.deepEqual(meter(tight, 1, ["a"]),
    ["kept", "installed", "installed", "installed", "cart", "free", "free", "full", "full", "full"]);
  assert.match(storageLine(tight, ["a"]), /^4 of 10 firmwares · about [\d.]+ MB free · room for 2 more$/);
});

test("applyOutcome: stops at the first failure and keeps it and the rest in the cart", () => {
  const bad = new Error("too full");
  assert.deepEqual(applyOutcome(["a", "b", "c"], ["added", bad]), { landed: ["a"], left: ["b", "c"], error: bad });
  assert.deepEqual(applyOutcome(["a", "b"], ["added", "already"]), { landed: ["a", "b"], left: [], error: null });
});

test("addError: the board's refusals in words that say what to do", () => {
  assert.match(addError(Object.assign(new Error("Blocked"), { code: 4 }), "SLOOP 2.8.4"), /Update Outrigger/);
  assert.match(addError(Object.assign(new Error("x"), { code: 5 }), "Felucca"), /re-released/);
  assert.equal(addError(new Error("Cable out."), "X0X 1.2.0"), "X0X 1.2.0 wasn't added. Cable out.");
});
