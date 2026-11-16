// faust_synth.c: a Vibe (built with the VIBE FM SDK) that plays the Faust patch voice.dsp, 8 voices.
// It mirrors examples/example-synth: notes from every source through one single-writer queue (the two flows),
// a free voice or the oldest one, the voices summed, a soft clip. faust_voice.h is `faust -lang c -cn voice voice.dsp`.
#include "vibe.h"
#include "vibe_dsp.h"
#include "vibe_gfx.h"
#include <math.h>
#include <stdlib.h>
#include "faust_glue.h"
#include "faust_voice.h"

VIBE_PARALLEL_AUDIO

#define VOICES 8
#define RELEASE_FRAMES(sr) ((uint32_t)((sr) / 2))   /* the patch's release is 0.4 s: a voice is free 0.5 s after its off */
typedef struct { voice dsp; int note; unsigned age; uint32_t off_left; int on; } fvoice;
static fvoice V[VOICES];
static int SRI;
static unsigned age;
static float buf[VIBE_MAX_BLOCK];
#define QN 64
static struct { int note; float vel; } q[QN];
static volatile uint32_t qw, qr;

static void queue_note(int note, float vel)   /* (control flow) full: dropped */
{
  uint32_t w = qw;
  if (w - qr >= QN) return;
  q[w % QN].note = note; q[w % QN].vel = vel;
  VIBE_RELEASE();
  qw = w + 1;
}

static void note_on(int note, float vel)
{
  int pick = 0;
  for (int i = 0; i < VOICES; i++) {
    if (!V[i].on && !V[i].off_left) { pick = i; break; }
    if (V[i].age < V[pick].age) pick = i;
  }
  fvoice *v = &V[pick];
  v->note = note; v->age = ++age; v->on = 1; v->off_left = 0;
  v->dsp.fHslider3 = vd_mtof((float)note);   /* freq */
  v->dsp.fHslider0 = vel;                    /* gain */
  v->dsp.fButton0 = 1.0f;                    /* gate */
}

static void note_off(int note)
{
  for (int i = 0; i < VOICES; i++)
    if (V[i].on && V[i].note == note) { V[i].on = 0; V[i].off_left = RELEASE_FRAMES(SRI); V[i].dsp.fButton0 = 0.0f; }
}

void vibe_on_init(float sample_rate)
{
  SRI = (int)sample_rate;
  vibe_notes(VIBE_NOTES);
  for (int i = 0; i < VOICES; i++) { initvoice(&V[i].dsp, SRI); V[i].note = -1; V[i].on = 0; V[i].off_left = 0; }
}

void vibe_on_event(const vibe_event *e)
{
  if (e->type == VIBE_EV_NOTE) queue_note(e->id, e->down ? (e->value > 0.0f ? e->value : 0.01f) : 0.0f);
}

void vibe_on_audio(const float *in, float *out_l, float *out_r, int frames)
{
  (void)in;
  uint32_t w = qw;
  VIBE_ACQUIRE();
  for (uint32_t k = qr; k != w; k++) { if (q[k % QN].vel > 0.0f) note_on(q[k % QN].note, q[k % QN].vel); else note_off(q[k % QN].note); }
  VIBE_RELEASE(); qr = w;
  for (int n = 0; n < frames; n++) out_l[n] = 0.0f;
  float *outs[1] = { buf };
  for (int i = 0; i < VOICES; i++) {
    fvoice *v = &V[i];
    if (!v->on && !v->off_left) continue;
    computevoice(&v->dsp, frames, 0, outs);
    for (int n = 0; n < frames; n++) out_l[n] += buf[n];
    if (!v->on) v->off_left = v->off_left > (uint32_t)frames ? v->off_left - (uint32_t)frames : 0;
  }
  const float vol = 0.7f * 0.25f;
  for (int n = 0; n < frames; n++) { float o = vd_tanh(out_l[n] * vol); out_l[n] = o; out_r[n] = o; }
}

void vibe_on_draw(uint16_t *fb)
{
  (void)fb;
  vg_clear(0x101114);
  vg_text("FAUST SYNTH", 16, 12, VG_BIG, 0xF2F2F2, 0);
}
