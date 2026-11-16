// Felucca's DSP for the P4 bench: lib/synth's translation unit, unchanged
// (-O2 -fwrapv, the long lines in PSRAM). The Makefile puts lib/synth/src on
// the include path; the sketch doesn't use lib/synth as a library, because its
// audio_out.cpp is Big-S3's ES8311 and needs that board's drivers.
#include "felucca_unit.c"
