/* the libc bits faust_voice.h asks for, for a wasm32-freestanding build (tools/build.sh adds -I<firmware>/freestanding).
   tanf comes from fm_math.c, compiled into the native build too, so both run the same maths. */
#pragma once
float tanf(float x);
#define fabsf(x) __builtin_fabsf(x)
#define fmaxf(a, b) __builtin_fmaxf(a, b)
#define fminf(a, b) __builtin_fminf(a, b)
