// The board's USB-Serial-JTAG port stalls about 30 s on a single write of
// 4 KiB or more (measured 2026-10-31: a 6406-byte stub block took 30.1 s,
// 1 KiB pieces took milliseconds). esptool-js writes whole SLIP frames, up
// to 16 KiB, so the page splits them (site/installer/serial.js).
import { test } from "node:test";
import assert from "node:assert/strict";
import { Transport } from "../../site/vendor/esptool-js-0.7.0/bundle.js";
import { ChunkedTransport, WRITE_CHUNK } from "../../site/installer/serial.js";

function fakePort() {
  const writes = [];
  return {
    writes,
    getInfo: () => ({ usbVendorId: 0x303a, usbProductId: 0x1001 }),
    writable: new WritableStream({ write: (c) => { writes.push(Uint8Array.from(c)); } }),
  };
}

test("every write to the port is at most WRITE_CHUNK bytes", async () => {
  const port = fakePort();
  const t = new ChunkedTransport(port);
  await t.write(new Uint8Array(16384).fill(0x42));
  assert.ok(port.writes.length > 1);
  assert.ok(port.writes.every((w) => w.length <= WRITE_CHUNK), `largest write ${Math.max(...port.writes.map((w) => w.length))}`);
  assert.ok(WRITE_CHUNK <= 1024);
});

test("the pieces add up to exactly the frame esptool-js would send", async () => {
  const data = Uint8Array.from({ length: 6160 }, (_, i) => (i * 7) & 0xff); // includes SLIP's 0xC0 and 0xDB
  const ours = fakePort();
  await new ChunkedTransport(ours).write(data);
  const theirs = fakePort();
  await new Transport(theirs).write(data);
  const joined = Uint8Array.from(ours.writes.flatMap((w) => [...w]));
  assert.deepEqual(joined, theirs.writes[0]);
});
