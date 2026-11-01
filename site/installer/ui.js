// What the page's install and add areas share: the board's one USB port, so
// only one of them uses it at a time, and the little element helper.

// The ESP32-S3's own USB port (USB-Serial-JTAG), in Computer mode or BOOT+RST.
export const PORT_FILTER = [{ usbVendorId: 0x303a, usbProductId: 0x1001 }];
// Web Serial's default 255-byte buffer can't take a flash-read packet (up to
// 4 KiB, more once SLIP-escaped): reads stalled after "Flash ID" in Brave.
// ESP Web Tools opened this board with 8192.
export const SERIAL_OPTIONS = { bufferSize: 16384 };
// One job at a time on the page: every area shares the one board.
export const port = { busy: false };

export const h = (tag, attrs = {}, ...kids) => {
  const el = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === "on") Object.entries(v).forEach(([ev, fn]) => el.addEventListener(ev, fn));
    else if (v !== false && v != null) el.setAttribute(k, v === true ? "" : v);
  }
  el.append(...kids.flat().filter((k) => k != null && k !== false));
  return el;
};
