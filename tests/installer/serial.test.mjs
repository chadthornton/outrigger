// The board's USB-Serial-JTAG port stalls about 30 s on a single write of
// 4 KiB or more (measured 2026-10-31: a 6406-byte stub block took 30.1 s,
// 1 KiB pieces took milliseconds). esptool-js writes whole SLIP frames, up
// to 16 KiB, so the page splits them (site/installer/serial.js).
import { test } from "node:test";
import assert from "node:assert/strict";
import { Transport } from "../../site/vendor/esptool-js-0.7.0/bundle.js";
import { ChunkedTransport, WRITE_CHUNK, readFlashInPieces, READ_BLOCK } from "../../site/installer/serial.js";

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

// A board running the flasher stub, as far as reading flash goes: READ_FLASH
// (0xd2) takes offset, length, packet size and packets in flight; the stub
// answers OK, then sends packets, sending more only as the host acknowledges
// the running total, and ends with the data's MD5.
function fakeStub(flash) {
  const sent = { commands: [], acks: [] };
  let pending = [];
  const loader = {
    ESP_READ_FLASH: 0xd2,
    FLASH_READ_TIMEOUT: 1000,
    _intToByteArray: (n) => [n & 0xff, (n >> 8) & 0xff, (n >> 16) & 0xff, (n >>> 24) & 0xff],
    async checkCommand(desc, op, data) {
      const v = new DataView(Uint8Array.from(data).buffer);
      const [addr, size, block, inFlight] = [0, 4, 8, 12].map((o) => v.getUint32(o, true));
      sent.commands.push({ op, addr, size, block, inFlight });
      let at = 0;
      const next = () => { // queue up to inFlight unacked packets
        while (pending.length < inFlight && at < size) {
          pending.push(flash.slice(addr + at, addr + Math.min(at + block, size)));
          at += block;
        }
        if (at >= size && !pending.length) pending.push(new Uint8Array(16)); // MD5
      };
      this.transport.onAck = (n) => { sent.acks.push(n); next(); };
      next();
      return 0;
    },
    transport: {
      async read() { if (!pending.length) throw new Error("timeout: nothing sent"); return pending.shift(); },
      async write(bytes) { const n = new DataView(Uint8Array.from(bytes).buffer).getUint32(0, true); this.onAck(n); },
    },
  };
  return { loader, sent };
}

test("readFlashInPieces: 1 KiB packets, one at a time, every byte in order", async () => {
  const flash = Uint8Array.from({ length: 0x20000 }, (_, i) => (i * 31) & 0xff);
  const { loader, sent } = fakeStub(flash);
  const progress = [];
  // esptool-js's readFlash callback shape, so the page can swap it in for loader.readFlash
  const got = await readFlashInPieces(loader, 0x8000, 0xc00 + 5, (packet, done, total) => progress.push([packet.length, done, total]));
  assert.deepEqual(got, flash.slice(0x8000, 0x8000 + 0xc05));
  assert.deepEqual(sent.commands, [{ op: 0xd2, addr: 0x8000, size: 0xc05, block: READ_BLOCK, inFlight: 1 }]);
  assert.ok(READ_BLOCK <= 64);
  assert.equal(sent.acks.length, Math.ceil(3077 / READ_BLOCK));
  assert.equal(sent.acks.at(-1), 3077);
  assert.deepEqual(progress.at(-1), [3077 % READ_BLOCK || READ_BLOCK, 3077, 3077]);
});
