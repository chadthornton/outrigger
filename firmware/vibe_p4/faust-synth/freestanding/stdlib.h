/* faust_synth.c never calls newvoice/deletevoice (no heap in a Vibe); these only let faust_voice.h compile */
#pragma once
#include <stddef.h>
#define calloc(n, s) ((void *)0)
#define free(p) ((void)(p))
