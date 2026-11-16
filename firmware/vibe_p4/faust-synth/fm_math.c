// fm_math.c: tanf for the Faust patch, one portable float version compiled into both the .wasm and the native build,
// so a WASM-vs-native comparison measures the runtime and not two different libms. The patch calls it once per block
// (the filter's coefficient), never per sample.
#include <stdint.h>

float tanf(float x)
{
  // reduce by k * pi/2 to [-pi/4, pi/4], a Taylor-series odd polynomial for tan, and -1/tan for odd k
  const float pio2 = 1.57079632679f;
  float kf = x * 0.636619772368f;
  int k = (int)(kf + (kf >= 0 ? 0.5f : -0.5f));
  float r = x - (float)k * pio2;
  float r2 = r * r;
  float t = r + r * r2 * (0.333333343f + r2 * (0.133333340f + r2 * (0.0539682540f + r2 * (0.0218694885f +
            r2 * (0.00886323553f + r2 * 0.00359212868f)))));
  return (k & 1) ? -1.0f / t : t;
}
