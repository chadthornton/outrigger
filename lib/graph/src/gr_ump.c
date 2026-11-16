#include "gr_ump.h"

int gr_ump_words(uint32_t w0) {
  static const uint8_t kWords[16] = {1, 1, 1, 2, 2, 4, 1, 1, 2, 2, 2, 3, 3, 4, 4, 4};
  return kWords[gr_ump_type(w0)];
}

int gr_ump_is_note_on(const uint32_t *w) {
  uint32_t t = gr_ump_type(w[0]);
  if (gr_ump_opcode(w[0]) != GR_OP_NOTE_ON) return 0;
  if (t == GR_UMP_M1_CV) return (w[0] & 0x7F) != 0;
  return t == GR_UMP_M2_CV;  // MIDI 2.0: velocity 0 is a real (silent) note on
}

int gr_ump_is_note_off(const uint32_t *w) {
  uint32_t t = gr_ump_type(w[0]);
  if (!gr_ump_is_cv(w[0])) return 0;
  if (gr_ump_opcode(w[0]) == GR_OP_NOTE_OFF) return 1;
  return t == GR_UMP_M1_CV && gr_ump_opcode(w[0]) == GR_OP_NOTE_ON && (w[0] & 0x7F) == 0;
}

uint32_t gr_ump_m2_note(uint32_t group, uint32_t op, uint32_t ch, uint32_t note, uint32_t vel16,
                        uint32_t out[2]) {
  out[0] = (GR_UMP_M2_CV << 28) | ((group & 0xF) << 24) | ((op & 0xF) << 20) | ((ch & 0xF) << 16) |
           ((note & 0x7F) << 8);
  out[1] = (vel16 & 0xFFFF) << 16;
  return out[0];
}

void gr_ump_m2_cc(uint32_t group, uint32_t ch, uint32_t index, uint32_t value32, uint32_t out[2]) {
  out[0] = (GR_UMP_M2_CV << 28) | ((group & 0xF) << 24) | (GR_OP_CC << 20) | ((ch & 0xF) << 16) |
           ((index & 0x7F) << 8);
  out[1] = value32;
}

uint32_t gr_ump_m1(uint32_t group, uint8_t status, uint8_t d1, uint8_t d2) {
  return (GR_UMP_M1_CV << 28) | ((group & 0xF) << 24) | ((uint32_t)status << 16) |
         ((uint32_t)(d1 & 0x7F) << 8) | (d2 & 0x7F);
}

uint32_t gr_ump_system(uint32_t group, uint8_t status, uint8_t d1, uint8_t d2) {
  return (GR_UMP_SYSTEM << 28) | ((group & 0xF) << 24) | ((uint32_t)status << 16) |
         ((uint32_t)(d1 & 0x7F) << 8) | (d2 & 0x7F);
}

// M2-104-UM 1.1, "Min-Center-Max" upscaling.
uint32_t gr_scale_up(uint32_t v, int src_bits, int dst_bits) {
  int shift = dst_bits - src_bits;
  uint32_t src_center = 1u << (src_bits - 1);
  if (shift <= 0) return v;
  uint32_t up = v << shift;
  if (v <= src_center) return up;
  int rep_bits = src_bits - 1;
  uint32_t rep = v & ((1u << rep_bits) - 1);
  if (shift > rep_bits) rep <<= shift - rep_bits; else rep >>= rep_bits - shift;
  while (rep != 0) {
    up |= rep;
    rep >>= rep_bits;
  }
  return up;
}

int gr_ump_from_midi1(const uint8_t *b, int len, uint32_t group, int proto2, uint32_t out[2]) {
  if (len < 1 || !(b[0] & 0x80)) return 0;
  uint8_t st = b[0];
  uint8_t d1 = len > 1 ? b[1] & 0x7F : 0;
  uint8_t d2 = len > 2 ? b[2] & 0x7F : 0;
  if (st >= 0xF0) {
    if (st == 0xF0 || st == 0xF7) return 0;  // SysEx goes as type 3, not here
    out[0] = gr_ump_system(group, st, d1, d2);
    return 1;
  }
  if (!proto2) {
    out[0] = gr_ump_m1(group, st, d1, d2);
    return 1;
  }
  uint32_t op = st >> 4, ch = st & 0xF;
  uint32_t w0 = (GR_UMP_M2_CV << 28) | ((group & 0xF) << 24) | (ch << 16);
  switch (op) {
    case GR_OP_NOTE_ON:
      if (d2 == 0) op = GR_OP_NOTE_OFF;  // spec: velocity 0 becomes a note off
      out[0] = w0 | (op << 20) | ((uint32_t)d1 << 8);
      out[1] = op == GR_OP_NOTE_OFF ? 0 : gr_scale_up(d2, 7, 16) << 16;
      return 2;
    case GR_OP_NOTE_OFF:
      out[0] = w0 | (op << 20) | ((uint32_t)d1 << 8);
      out[1] = gr_scale_up(d2, 7, 16) << 16;
      return 2;
    case GR_OP_POLY_PRESSURE:
    case GR_OP_CC:
      out[0] = w0 | (op << 20) | ((uint32_t)d1 << 8);
      out[1] = gr_scale_up(d2, 7, 32);
      return 2;
    case GR_OP_PROGRAM:
      out[0] = w0 | (op << 20);
      out[1] = (uint32_t)d1 << 24;
      return 2;
    case GR_OP_CHAN_PRESSURE:
      out[0] = w0 | (op << 20);
      out[1] = gr_scale_up(d1, 7, 32);
      return 2;
    case GR_OP_PITCH_BEND:
      out[0] = w0 | (op << 20);
      out[1] = gr_scale_up(((uint32_t)d2 << 7) | d1, 14, 32);
      return 2;
  }
  return 0;
}

int gr_ump_to_midi1(const uint32_t *w, uint8_t out[3]) {
  uint32_t t = gr_ump_type(w[0]);
  uint8_t st = (uint8_t)gr_ump_status(w[0]);
  uint8_t d1 = (w[0] >> 8) & 0x7F, d2 = w[0] & 0x7F;
  if (t == GR_UMP_SYSTEM) {
    out[0] = st;
    out[1] = d1;
    out[2] = d2;
    if (st == 0xF1 || st == 0xF3) return 2;
    if (st == 0xF2) return 3;
    return 1;
  }
  if (t == GR_UMP_M1_CV) {
    out[0] = st;
    out[1] = d1;
    out[2] = d2;
    return (st >> 4) == GR_OP_PROGRAM || (st >> 4) == GR_OP_CHAN_PRESSURE ? 2 : 3;
  }
  if (t != GR_UMP_M2_CV) return 0;
  uint32_t op = gr_ump_opcode(w[0]);
  uint8_t ch = (uint8_t)gr_ump_channel(w[0]);
  out[0] = (uint8_t)((op << 4) | ch);
  switch (op) {
    case GR_OP_NOTE_ON: {
      uint32_t v = gr_scale_down(w[1] >> 16, 16, 7);
      out[1] = d1;
      out[2] = (uint8_t)(v ? v : 1);  // spec: never let a note on become a note off
      return 3;
    }
    case GR_OP_NOTE_OFF:
      out[1] = d1;
      out[2] = (uint8_t)gr_scale_down(w[1] >> 16, 16, 7);
      return 3;
    case GR_OP_POLY_PRESSURE:
    case GR_OP_CC:
      out[1] = d1;
      out[2] = (uint8_t)gr_scale_down(w[1], 32, 7);
      return 3;
    case GR_OP_PROGRAM:
      out[1] = (uint8_t)((w[1] >> 24) & 0x7F);
      return 2;
    case GR_OP_CHAN_PRESSURE:
      out[1] = (uint8_t)gr_scale_down(w[1], 32, 7);
      return 2;
    case GR_OP_PITCH_BEND: {
      uint32_t v = gr_scale_down(w[1], 32, 14);
      out[1] = v & 0x7F;
      out[2] = (v >> 7) & 0x7F;
      return 3;
    }
  }
  return 0;  // registered / per-note controllers have no MIDI 1.0 form here
}
