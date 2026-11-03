#include "board_blob.h"

#include <initializer_list>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace {

// Little-endian cursor over a fixed buffer.
struct Writer {
  uint8_t *p;
  void u8(uint8_t v) { *p++ = v; }
  void i8(int8_t v) { *p++ = uint8_t(v); }
  void u16(uint16_t v) { u8(uint8_t(v)); u8(uint8_t(v >> 8)); }
  void i16(int16_t v) { u16(uint16_t(v)); }
  void u32(uint32_t v) { u16(uint16_t(v)); u16(uint16_t(v >> 16)); }
};
struct Reader {
  const uint8_t *p;
  uint8_t u8() { return *p++; }
  int8_t i8() { return int8_t(*p++); }
  uint16_t u16() { const uint16_t lo = u8(); return uint16_t(lo | uint16_t(u8()) << 8); }
  int16_t i16() { return int16_t(u16()); }
  uint32_t u32() { const uint32_t lo = u16(); return lo | uint32_t(u16()) << 16; }
};

}  // namespace

uint16_t board_crc16(const uint8_t *p, size_t n) {
  uint16_t crc = 0;
  while (n--) {
    crc ^= uint16_t(*p++) << 8;
    for (int i = 0; i < 8; ++i) crc = crc & 0x8000 ? uint16_t((crc << 1) ^ 0x1021) : uint16_t(crc << 1);
  }
  return crc;
}

BoardProfile BoardBlob::profile() const {
  BoardProfile p{name, width, height, shape, corner_radius, font_boost};
  return p;
}

size_t board_blob_encode(const BoardBlob &b, uint8_t *out, size_t out_len) {
  if (out_len < kBlobSize) return 0;
  const BoardConfig &c = b.config;
  Writer w{out};
  w.u8('O'); w.u8('B'); w.u8('P'); w.u8(kBlobVersion);
  w.u16(uint16_t(kBlobSize));
  w.u8(b.tag_id);
  w.u8(uint8_t((c.ips ? 1 : 0) | (c.restart_on_power_on ? 2 : 0) | (b.quad_psram ? 4 : 0) | (uint8_t(b.shape) & 3) << 3));
  w.u16(b.width); w.u16(b.height); w.u16(b.corner_radius); w.u8(b.font_boost);
  w.u8(uint8_t(c.panel)); w.u8(uint8_t(c.touch)); w.u8(uint8_t(c.expander));
  for (int16_t pin : {c.lcd_dc, c.lcd_cs, c.lcd_sck, c.lcd_mosi, c.lcd_miso, c.lcd_rst, c.lcd_bl}) w.i16(pin);
  w.u32(c.spi_hz);
  w.i8(c.spi_mode); w.u8(c.rotation); w.i8(c.bgr); w.i8(c.mirror_x); w.i8(c.mirror_y);
  for (int16_t o : c.offsets) w.i16(o);
  w.u8(c.touch_address);
  for (int16_t pin : {c.tp_sda, c.tp_scl, c.tp_rst, c.tp_int}) w.i16(pin);
  w.i8(c.touch_swap_xy); w.i8(c.touch_invert_x); w.i8(c.touch_invert_y);
  w.u8(c.expander_address);
  for (int16_t pin : c.hold_low) w.i16(pin);
  for (int16_t pin : c.hold_high) w.i16(pin);
  memcpy(w.p, b.name, kBlobNameMax);
  w.p += kBlobNameMax;
  if (size_t(w.p - out) != kBlobSize - 2) return 0;  // the layout and kBlobSize disagree
  w.u16(board_crc16(out, kBlobSize - 2));
  return kBlobSize;
}

BlobError board_blob_decode(const uint8_t *data, size_t len, BoardBlob &b) {
  if (len != kBlobSize || data[0] != 'O' || data[1] != 'B' || data[2] != 'P' || data[3] != kBlobVersion)
    return BlobError::Format;
  Reader r{data + 4};
  if (r.u16() != kBlobSize) return BlobError::Format;
  const uint16_t want = uint16_t(data[kBlobSize - 2] | data[kBlobSize - 1] << 8);
  if (board_crc16(data, kBlobSize - 2) != want) return BlobError::Crc;
  b = BoardBlob{};
  BoardConfig &c = b.config;
  b.tag_id = r.u8();
  const uint8_t flags = r.u8();
  c.ips = flags & 1;
  c.restart_on_power_on = flags & 2;
  b.quad_psram = flags & 4;
  const uint8_t shape = (flags >> 3) & 3;
  if (shape > uint8_t(ScreenShape::Circle)) return BlobError::Screen;
  b.shape = ScreenShape(shape);
  b.width = r.u16(); b.height = r.u16(); b.corner_radius = r.u16(); b.font_boost = r.u8();
  const uint8_t panel = r.u8(), touch = r.u8(), expander = r.u8();
  if (panel > uint8_t(PanelDriver::ILI9341) || touch > uint8_t(TouchDriver::FT6336) ||
      expander > uint8_t(ExpanderDriver::TCA9554))
    return BlobError::Driver;
  c.panel = PanelDriver(panel); c.touch = TouchDriver(touch); c.expander = ExpanderDriver(expander);
  for (int16_t *pin : {&c.lcd_dc, &c.lcd_cs, &c.lcd_sck, &c.lcd_mosi, &c.lcd_miso, &c.lcd_rst, &c.lcd_bl}) *pin = r.i16();
  c.spi_hz = r.u32();
  c.spi_mode = r.i8(); c.rotation = r.u8(); c.bgr = r.i8(); c.mirror_x = r.i8(); c.mirror_y = r.i8();
  for (int16_t &o : c.offsets) o = r.i16();
  c.touch_address = r.u8();
  for (int16_t *pin : {&c.tp_sda, &c.tp_scl, &c.tp_rst, &c.tp_int}) *pin = r.i16();
  c.touch_swap_xy = r.i8(); c.touch_invert_x = r.i8(); c.touch_invert_y = r.i8();
  c.expander_address = r.u8();
  for (int16_t &pin : c.hold_low) pin = r.i16();
  for (int16_t &pin : c.hold_high) pin = r.i16();
  memcpy(b.name, r.p, kBlobNameMax);
  if (b.name[0] == 0 || memchr(b.name, 0, kBlobNameMax) == nullptr) return BlobError::Name;
  return BlobError::Ok;
}

namespace {

constexpr int16_t kExio = 100;

struct Check {
  const BoardBlob &b;
  char *why;
  size_t why_len;
  struct Used {
    int16_t pin;
    const char *where;
  } used[24];
  int n_used = 0;

  BlobError fail(BlobError e, const char *fmt, ...) {
    if (why && why_len) {
      va_list ap;
      va_start(ap, fmt);
      vsnprintf(why, why_len, fmt, ap);
      va_end(ap);
    }
    return e;
  }

  // As boards.py check_board's use(): the S3's GPIOs, off USB, flash and octal PSRAM, once each.
  BlobError pin(const char *where, int16_t p, bool optional) {
    if (p < 0) return optional ? BlobError::Ok : fail(BlobError::PinMissing, "%s needs a pin", where);
    if (p >= kExio) {
      if (p > kExio + 7) return fail(BlobError::PinNotOnChip, "%s is on expander pin %d", where, p - kExio);
      if (b.config.expander == ExpanderDriver::None)
        return fail(BlobError::ExioNoExpander, "%s is on exio%d, but the board has no expander", where, p - kExio);
    } else {
      if (p > 48 || (p >= 22 && p <= 25)) return fail(BlobError::PinNotOnChip, "%s is on GPIO%d, which the ESP32-S3 doesn't have", where, p);
      if (p == 19 || p == 20) return fail(BlobError::PinUsb, "%s is on GPIO%d, a USB host line", where, p);
      if (p >= 26 && p <= 32) return fail(BlobError::PinFlash, "%s is on GPIO%d, a flash line", where, p);
      if (p >= 33 && p <= 37 && !b.quad_psram) return fail(BlobError::PinPsram, "%s is on GPIO%d, an octal-PSRAM line", where, p);
    }
    for (int i = 0; i < n_used; ++i)
      if (used[i].pin == p) return fail(BlobError::PinTwice, "%s and %s share a pin", used[i].where, where);
    if (n_used < int(sizeof used / sizeof *used)) used[n_used++] = {p, where};
    return BlobError::Ok;
  }
};

bool tri(int8_t v) { return v >= -1 && v <= 1; }

}  // namespace

BlobError board_blob_check(const BoardBlob &b, char *why, size_t why_len) {
  if (why && why_len) why[0] = 0;
  Check k{b, why, why_len, {}, 0};
  const BoardConfig &c = b.config;
  if (b.width == 0 || b.height == 0 || b.width > 480 || b.height > 480)
    return k.fail(BlobError::Screen, "the screen is %ux%u; up to 480 on a side so far", unsigned(b.width), unsigned(b.height));
  if (b.shape == ScreenShape::Rounded && b.corner_radius == 0)
    return k.fail(BlobError::Screen, "a rounded screen needs its corner radius");
  if (c.rotation > 3 || c.spi_mode < -1 || c.spi_mode > 3 || !tri(c.bgr) || !tri(c.mirror_x) || !tri(c.mirror_y) ||
      !tri(c.touch_swap_xy) || !tri(c.touch_invert_x) || !tri(c.touch_invert_y) || b.font_boost > 4 || c.spi_hz == 0)
    return k.fail(BlobError::Field, "a field is out of range (rotation, SPI mode or speed, a yes/no, font boost)");
  if (c.touch_address < 0x08 || c.touch_address > 0x77 ||
      (c.expander != ExpanderDriver::None && (c.expander_address < 0x08 || c.expander_address > 0x77)))
    return k.fail(BlobError::I2cAddress, "an I2C address is outside 0x08-0x77");
  struct {
    const char *where;
    int16_t pin;
    bool optional;
  } const pins[] = {{"display.dc", c.lcd_dc, false},   {"display.cs", c.lcd_cs, true},
                    {"display.sck", c.lcd_sck, false}, {"display.mosi", c.lcd_mosi, false},
                    {"display.miso", c.lcd_miso, true}, {"display.rst", c.lcd_rst, true},
                    {"display.backlight", c.lcd_bl, false}, {"touch.sda", c.tp_sda, false},
                    {"touch.scl", c.tp_scl, false},   {"touch.rst", c.tp_rst, true},
                    {"touch.int", c.tp_int, true}};
  for (const auto &p : pins)
    if (BlobError e = k.pin(p.where, p.pin, p.optional); e != BlobError::Ok) return e;
  for (int16_t p : c.hold_low)
    if (p >= 0)
      if (BlobError e = k.pin("hold_low", p, false); e != BlobError::Ok) return e;
  for (int16_t p : c.hold_high)
    if (p >= 0)
      if (BlobError e = k.pin("hold_high", p, false); e != BlobError::Ok) return e;
  return BlobError::Ok;
}
