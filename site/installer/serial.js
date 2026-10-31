// esptool-js's Transport, writing in pieces. The ESP32-S3's USB-Serial-JTAG
// port stalls about 30 s on a single write of 4 KiB or more (measured on the
// Waveshare, 2026-10-31: a 6406-byte stub block took 30.1 s, 1 KiB pieces
// took milliseconds), and esptool-js writes whole SLIP frames of up to
// 16 KiB. tests/installer/serial.test.mjs.
import { Transport } from "../vendor/esptool-js-0.7.0/bundle.js";

export const WRITE_CHUNK = 1024;

export class ChunkedTransport extends Transport {
  async write(data) {
    const out = this.slipWriter(data);
    if (!this.device.writable) return;
    const writer = this.device.writable.getWriter();
    try {
      for (let i = 0; i < out.length; i += WRITE_CHUNK) await writer.write(out.subarray(i, i + WRITE_CHUNK));
    } finally {
      writer.releaseLock();
    }
  }
}

// esptool-js's readFlash asks the stub for 4 KiB packets, up to 1024 ahead of
// the host's acknowledgements. In Brave the board stopped mid-packet on the
// first 3 KiB read (2026-10-31), the read-side twin of the write stall. This
// asks for 1 KiB packets, one at a time: the stub sends the next only after
// the host acknowledges the running total, then ends with the data's MD5.
export const READ_BLOCK = 1024;

export async function readFlashInPieces(loader, addr, size, onProgress = () => {}) {
  const int = (n) => loader._intToByteArray(n);
  const res = await loader.checkCommand("read flash", loader.ESP_READ_FLASH,
    Uint8Array.from([...int(addr), ...int(size), ...int(READ_BLOCK), ...int(1)]));
  if (res != 0) throw new Error(`Failed to read flash: ${res}`);
  const out = new Uint8Array(size);
  let got = 0;
  while (got < size) {
    const packet = await loader.transport.read(loader.FLASH_READ_TIMEOUT);
    if (!(packet instanceof Uint8Array) || !packet.length) throw new Error(`Failed to read flash at ${got} of ${size}`);
    out.set(packet.subarray(0, size - got), got);
    got += packet.length;
    await loader.transport.write(Uint8Array.from(int(Math.min(got, size))));
    onProgress(Math.min(got, size), size);
  }
  await loader.transport.read(loader.FLASH_READ_TIMEOUT); // the stub's MD5 of what it sent
  return out;
}
