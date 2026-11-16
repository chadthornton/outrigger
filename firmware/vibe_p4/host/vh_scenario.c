// vh_scenario.c: the fixed note scenario, the output hash and the per-block timing. Shared by the Mac and the P4.
#include "vh.h"
#include <string.h>
#include <math.h>

const int vh_scenario_notes[8] = { 48, 52, 55, 59, 62, 65, 69, 72 };

static uint32_t fnv(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }

int vh_run(vh_backend *b, const vh_scenario *s, vh_clock_ns clock, vh_result *r, float *out_l, float *out_r)
{
  memset(r, 0, sizeof *r);
  if (b->init(b, s->sample_rate)) return -1;
  const int block = s->block;
  const uint32_t total = (uint32_t)ceil((double)s->seconds * s->sample_rate / block);
  int on_sent = 0, off_sent = 0;
  uint32_t h = 2166136261u;
  double sum = 0.0, sum_t = 0.0, sum_held = 0.0;
  uint32_t held = 0;
  for (uint32_t k = 0; k < total; k++) {
    const double t = (double)k * block / s->sample_rate;
    const uint64_t frame = (uint64_t)k * (uint64_t)block;
    if (!on_sent && t >= 0.0) {
      for (int i = 0; i < s->notes; i++)
        b->event(b, 5, 0, 0, 0.0f, 0x90 | (vh_scenario_notes[i] << 8) | (100 << 16), frame);
      on_sent = 1;
    }
    if (!off_sent && t >= s->off_at) {
      for (int i = 0; i < s->notes; i++)
        b->event(b, 5, 0, 0, 0.0f, 0x80 | (vh_scenario_notes[i] << 8), frame);
      off_sent = 1;
    }
    const float *l = 0, *rr = 0;
    const uint64_t t0 = clock ? clock() : 0;
    if (b->audio(b, block, &l, &rr)) return -2;
    const uint64_t t1 = clock ? clock() : 0;
    if (clock) {
      const double us = (double)(t1 - t0) / (s->ticks_per_us > 0 ? s->ticks_per_us : 1000.0);
      sum_t += us;
      if (us > r->us_max) { r->us_max = us; r->us_max_block = k; }
      if (!off_sent) { sum_held += us; held++; }
    }
    for (int i = 0; i < block; i++) {
      float a = l[i], c = rr[i];
      uint32_t ba, bc;
      memcpy(&ba, &a, 4); memcpy(&bc, &c, 4);
      h = fnv(h, ba); h = fnv(h, bc);
      if (!(a == a) || !(c == c) || a - a != 0.0f || c - c != 0.0f) r->nan++;
      float pa = a < 0 ? -a : a, pc = c < 0 ? -c : c;
      if (pa > r->peak) r->peak = pa;
      if (pc > r->peak) r->peak = pc;
      sum += (double)a * a;
      if (out_l) out_l[(size_t)k * block + i] = a;
      if (out_r) out_r[(size_t)k * block + i] = c;
    }
  }
  r->blocks = total;
  r->out_hash = h;
  r->rms = (float)sqrt(sum / ((double)total * block));
  if (clock) { r->us_mean = sum_t / total; r->us_mean_held = held ? sum_held / held : 0.0; }
  return 0;
}
