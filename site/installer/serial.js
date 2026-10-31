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
