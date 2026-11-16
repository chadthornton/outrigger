// MIDI 2.0 Universal MIDI Packet helpers (spike, night 0, C1-1).
//
// The graph carries every event as a UMP (1 to 4 32-bit words) plus a frame
// stamp inside the block. Channel voice messages may be MIDI 1.0 protocol
// (message type 0x2) or MIDI 2.0 protocol (type 0x4); both keep the opcode in
// bits 23..20, the channel in 19..16 and the note / controller index in 15..8
// of word 0, so the routing nodes read those fields the same way for either.
// Conversions follow the M2-104-UM spec's min-center-max scaling.
#ifndef GR_UMP_H
#define GR_UMP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  GR_UMP_UTILITY = 0x0,
  GR_UMP_SYSTEM = 0x1,  // system common and real-time (clock, start, stop)
  GR_UMP_M1_CV = 0x2,   // MIDI 1.0 channel voice
  GR_UMP_SYSEX7 = 0x3,
  GR_UMP_M2_CV = 0x4,   // MIDI 2.0 channel voice
  GR_UMP_DATA128 = 0x5,
};

// Opcodes (the status nibble).
enum {
  GR_OP_NOTE_OFF = 0x8,
  GR_OP_NOTE_ON = 0x9,
  GR_OP_POLY_PRESSURE = 0xA,
  GR_OP_CC = 0xB,
  GR_OP_PROGRAM = 0xC,
  GR_OP_CHAN_PRESSURE = 0xD,
  GR_OP_PITCH_BEND = 0xE,
};

static inline uint32_t gr_ump_type(uint32_t w0) { return w0 >> 28; }
static inline uint32_t gr_ump_group(uint32_t w0) { return (w0 >> 24) & 0xF; }
static inline uint32_t gr_ump_opcode(uint32_t w0) { return (w0 >> 20) & 0xF; }
static inline uint32_t gr_ump_channel(uint32_t w0) { return (w0 >> 16) & 0xF; }
static inline uint32_t gr_ump_index(uint32_t w0) { return (w0 >> 8) & 0xFF; }  // note or CC
static inline uint32_t gr_ump_status(uint32_t w0) { return (w0 >> 16) & 0xFF; }  // type 1 and 2

// Words in a packet of this message type (UMP spec table 1).
int gr_ump_words(uint32_t w0);

// Channel voice of either protocol.
static inline int gr_ump_is_cv(uint32_t w0) {
  uint32_t t = gr_ump_type(w0);
  return t == GR_UMP_M1_CV || t == GR_UMP_M2_CV;
}
// Note on with a non-zero velocity (a MIDI 1.0 note on at velocity 0 is a note off).
int gr_ump_is_note_on(const uint32_t *w);
// Note off, including the MIDI 1.0 note on at velocity 0.
int gr_ump_is_note_off(const uint32_t *w);

static inline uint32_t gr_ump_with_channel(uint32_t w0, uint32_t ch) {
  return (w0 & ~0x000F0000u) | ((ch & 0xF) << 16);
}
static inline uint32_t gr_ump_with_index(uint32_t w0, uint32_t idx) {
  return (w0 & ~0x0000FF00u) | ((idx & 0xFF) << 8);
}

// Builders. vel16 is MIDI 2.0's 16-bit velocity; value32 the 32-bit CC value.
uint32_t gr_ump_m2_note(uint32_t group, uint32_t op, uint32_t ch, uint32_t note, uint32_t vel16,
                        uint32_t out[2]);
void gr_ump_m2_cc(uint32_t group, uint32_t ch, uint32_t index, uint32_t value32, uint32_t out[2]);
uint32_t gr_ump_m1(uint32_t group, uint8_t status, uint8_t d1, uint8_t d2);
uint32_t gr_ump_system(uint32_t group, uint8_t status, uint8_t d1, uint8_t d2);

// Spec scaling between bit depths.
uint32_t gr_scale_up(uint32_t v, int src_bits, int dst_bits);
static inline uint32_t gr_scale_down(uint32_t v, int src_bits, int dst_bits) {
  return v >> (src_bits - dst_bits);
}

// MIDI 1.0 bytes (one message, 1 to 3 bytes, no SysEx) to a UMP. proto2 picks
// MIDI 2.0 protocol (type 4) for channel voice; system messages are always
// type 1. Returns the word count written to out (0 if not representable).
int gr_ump_from_midi1(const uint8_t *b, int len, uint32_t group, int proto2, uint32_t out[2]);

// A UMP back to MIDI 1.0 bytes. Returns the byte count (0: no MIDI 1.0 form,
// e.g. SysEx, utility, or a MIDI 2.0 per-note message).
int gr_ump_to_midi1(const uint32_t *w, uint8_t out[3]);

#ifdef __cplusplus
}
#endif

#endif
