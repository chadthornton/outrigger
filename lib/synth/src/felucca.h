// Felucca's synth, through the fm1-cartridge port's interface (fm1_synth.h).
#pragma once
#include <stdbool.h>

#include "../third_party/fm1-cartridge/synth/felucca_synth.h"
#include "../third_party/fm1-cartridge/synth/fm1_synth.h"

#ifdef __cplusplus
extern "C" {
#endif
// Allocates Felucca's delay and looper lines in PSRAM (felucca_unit.c). Call
// it once before the first init; false if PSRAM is short.
bool felucca_memory_begin(void);
#ifdef __cplusplus
}
#endif
