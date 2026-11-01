// Can this ESP32 run Outrigger? What the page checks right after esptool
// connects, before it reads or writes anything: the chip, its built-in PSRAM,
// its flash size, and whether the port is the chip's own USB or a converter.
// Pure: area.js passes in what esptool-js reported, so `make test` runs this
// on the Mac (tests/installer/chip.test.mjs, cases in chips.json).
//
// Outrigger is the USB host on the FM-1's hub, so the chip must be able to
// host USB through its own USB pins (docs/boards.md, "Will my board work?").

// USB-to-serial converter chips, by USB vendor ID. A board whose port goes
// through one talks to a computer fine but can never host.
export const USB_BRIDGES = {
  0x1a86: "a WCH converter chip (CH340 or CH343)",
  0x10c4: "a Silicon Labs converter chip (CP2102 or similar)",
  0x0403: "an FTDI converter chip",
};

const NO_HOST = ["ESP32-C2", "ESP32-C3", "ESP32-C5", "ESP32-C6", "ESP32-C61", "ESP32-H2", "ESP32-H21", "ESP32-H4", "ESP8266"];

// "ESP32-S3 (QFN56) (revision v0.2)" -> "ESP32-S3"; "ESP32-D0WD-V3 (revision v3.1)" -> "ESP32".
export function chipFamily(description) {
  const m = /^(ESP8266|ESP32(?:-(?:S2|S3|S31|P4|C2|C3|C5|C6|C61|H2|H21|H4))?)(?![0-9])/.exec(description || "");
  return m ? m[1] : null;
}

// The JEDEC ID esptool reads: its top byte is the capacity as a power of two
// (some makers offset it). Returns bytes, or null when the chip didn't answer.
export function flashBytesFromId(id) {
  const code = (id >> 16) & 0xff;
  if (code >= 0x12 && code <= 0x1c) return 2 ** code;
  if (code >= 0x20 && code <= 0x22) return 2 ** (code - 6);
  if (code >= 0x32 && code <= 0x3c) return 2 ** (code - 0x20);
  return null;
}

// esptool-js's feature list ("Embedded PSRAM 8MB (AP_3v3)") -> MB, 0 for none.
export function psramMbFromFeatures(features) {
  for (const f of features || []) {
    const m = /PSRAM (\d+)MB/.exec(f);
    if (m) return Number(m[1]);
  }
  return 0;
}

// "A, B or C"
const anyOf = (names) => (names.length < 2 ? names.join("") : `${names.slice(0, -1).join(", ")} or ${names.at(-1)}`);
const mb = (bytes) => `${bytes / 1048576} MB`;

// facts: {chip, features, flashId, usbVendorId} from esptool-js and the port.
// picked: the profile the user chose ({board, flash_mb, psram_mb, tested}),
// or null for "another board". profiles: every board the site builds.
// Returns {ok, words}: ok means installing the picked board's build is safe
// as far as the chip can tell; words say why, in plain English.
export function checkChip(facts, picked, profiles) {
  const family = chipFamily(facts.chip);
  const suggest = `You need an ESP32-S3 board, such as the ${anyOf(profiles.map((p) => p.board))}.`;
  const bridge = USB_BRIDGES[facts.usbVendorId];
  const named = family || `an ESP32 this page doesn't know (${facts.chip || "it didn't say which"})`;

  if (family === "ESP32") {
    return { ok: false, words: "This is an original ESP32, the chip on the \"Cheap Yellow Display\" (ESP32-2432S028) and most ESP32 " +
      "dev boards. It can't act as a USB host, so it can't run Outrigger: Outrigger plays your FM-1 by being the USB host on " +
      `its hub. ${suggest}` };
  }
  if (NO_HOST.includes(family)) {
    return { ok: false, words: `This is an ${family}. Its USB port can only be a device, never a USB host, so it can't run ` +
      `Outrigger: Outrigger plays your FM-1 by being the USB host on its hub. ${suggest}` };
  }
  if (family === "ESP32-S2") {
    return { ok: false, words: "This is an ESP32-S2. It can act as a USB host, but Outrigger doesn't run on it yet: the S2 " +
      `has one core and less memory than the S3, and there's no build for it. ${suggest}` };
  }
  if (family === "ESP32-P4") {
    return { ok: false, words: "This is an ESP32-P4. It can act as a USB host, but there's no Outrigger build for it yet " +
      `(its Wi-Fi sits on a second chip, and USB hubs on its fast port still need testing). For now, ${suggest[0].toLowerCase()}${suggest.slice(1)}` };
  }
  if (family !== "ESP32-S3") {
    return { ok: false, words: `This is ${named}. Outrigger runs only on the ESP32-S3 so far. ${suggest}` };
  }

  if (bridge) {
    return { ok: false, words: `This ESP32-S3 is plugged in through ${bridge}. That port can't act as a USB host, and ` +
      "Outrigger needs it to. If the board has a second USB-C port marked USB, OTG or native, plug into that one and try " +
      "again. If this is its only port, the board can't run Outrigger." };
  }

  const flash = flashBytesFromId(facts.flashId);
  const psram = psramMbFromFeatures(facts.features);
  if (flash === null) {
    return { ok: false, words: "This is an ESP32-S3, but its flash chip didn't say how big it is, so nothing was written. " +
      "Unplug it, plug it back in, and try again." };
  }
  const short = [];
  if (flash < 16 * 1048576) {
    short.push(`It has ${mb(flash)} of flash; Outrigger needs 16 MB, for the program, its recovery copy and the store that ` +
      "holds your FM-1 backups and firmwares.");
  }
  if (psram < 8) {
    short.push((psram ? `It has ${psram} MB of PSRAM` : "Its chip has no built-in PSRAM") + "; Outrigger needs 8 MB, to hold " +
      "a whole FM-1 firmware while it switches.");
  }
  if (short.length) {
    return { ok: false, words: `This ESP32-S3 is too small for Outrigger. ${short.join(" ")} Boards listed as "N16R8" have ` +
      "both. The MIDI hub on its own would fit a smaller board, but there's no build for that yet: tell us if you'd use one." };
  }

  const fits = `An ESP32-S3 with ${mb(flash)} of flash and ${psram} MB of PSRAM`;
  if (!picked) {
    return { ok: false, words: `${fits}: enough for Outrigger. But there's no profile for this board yet, so there's nothing ` +
      "safe to install: a build for another board drives pins your board may use for something else. A profile is a short " +
      "list of the board's pins, taken from its maker's schematic. docs/boards.md (\"Adding a board\") in Outrigger's " +
      "GitHub repo says how, or send us a link to the board and we'll look." };
  }
  const untested = picked.tested === false
    ? ` Nobody has run Outrigger on a ${picked.board} yet; it should work. Tell us how it goes.` : "";
  return { ok: true, words: `${fits}, as the ${picked.board} has. The chip can't say which board it sits on, so check this ` +
    `is a ${picked.board}.${untested}` };
}
