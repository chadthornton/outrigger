// The app checker's harness. See gr_check.h and lib/graph/CONTRACT.md.
#if !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "gr_check.h"

#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "gr_ump.h"

// ---- the report ----

static void add(gr_ck_report *r, const char *part, int ok, const char *fmt, ...) {
  if (r->n >= (int)(sizeof(r->line) / sizeof(r->line[0]))) return;
  gr_ck_line *l = &r->line[r->n++];
  snprintf(l->part, sizeof(l->part), "%s", part);
  l->ok = ok;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(l->text, sizeof(l->text), fmt, ap);
  va_end(ap);
  if (ok == 0) r->fails++;
}

int gr_ck_failed(const gr_ck_report *r, const char *part) {
  for (int i = 0; i < r->n; ++i)
    if (r->line[i].ok == 0 && strcmp(r->line[i].part, part) == 0) return 1;
  return 0;
}

void gr_ck_defaults(gr_ck_options *o) {
  memset(o, 0, sizeof(*o));
  o->seconds = 2.0;
  o->seed = 1;
  o->isolate = 1;
  o->timeout_s = 60;
}

// ---- the fuzzed input ----

typedef struct ck_ev {
  uint64_t frame;
  uint32_t order;
  uint32_t ump[4];
} ck_ev;

typedef struct ck_script {
  ck_ev *ev;
  int n, cap;
} ck_script;

static uint32_t rnd_next(uint32_t *s) {
  *s ^= *s << 13;
  *s ^= *s >> 17;
  *s ^= *s << 5;
  return *s;
}

static void script_add(ck_script *sc, uint64_t frame, const uint32_t *w) {
  if (sc->n == sc->cap) {
    sc->cap = sc->cap ? sc->cap * 2 : 1024;
    sc->ev = (ck_ev *)realloc(sc->ev, sizeof(ck_ev) * (size_t)sc->cap);
  }
  ck_ev *e = &sc->ev[sc->n];
  e->frame = frame;
  e->order = (uint32_t)sc->n;
  memset(e->ump, 0, sizeof(e->ump));
  memcpy(e->ump, w, sizeof(uint32_t) * (size_t)gr_ump_words(w[0]));
  sc->n++;
}

static int ev_cmp(const void *a, const void *b) {
  const ck_ev *x = (const ck_ev *)a, *y = (const ck_ev *)b;
  if (x->frame != y->frame) return x->frame < y->frame ? -1 : 1;
  return x->order < y->order ? -1 : (x->order > y->order);
}

// Every kind of message, at random frames (some on the same frame), every
// note on with its note off inside `frames`.
static void script_make(ck_script *sc, uint32_t seed, uint64_t frames) {
  uint32_t s = seed * 2654435761u + 12345u;
  if (!s) s = 1;
  uint64_t t = 0;
  for (;;) {
    uint32_t r = rnd_next(&s);
    t += (r & 7) == 0 ? 0 : (r >> 3) % 90;
    if (t + 4000 >= frames) break;
    uint32_t w[4] = {0, 0, 0, 0};
    uint32_t kind = (r >> 10) % 100, ch = (r >> 17) & 15, note = (r >> 21) & 127;
    uint32_t v = 1 + rnd_next(&s) % 127;
    if (kind < 35) {  // MIDI 1.0 note, and its off (sometimes a vel-0 note on)
      w[0] = gr_ump_m1(0, (uint8_t)(0x90 | ch), (uint8_t)note, (uint8_t)v);
      script_add(sc, t, w);
      w[0] = (r & 1) ? gr_ump_m1(0, (uint8_t)(0x80 | ch), (uint8_t)note, 64)
                     : gr_ump_m1(0, (uint8_t)(0x90 | ch), (uint8_t)note, 0);
      script_add(sc, t + 1 + rnd_next(&s) % 3000, w);
    } else if (kind < 50) {  // MIDI 2.0 note, and its off
      gr_ump_m2_note(0, GR_OP_NOTE_ON, ch, note, v << 9, w);
      script_add(sc, t, w);
      gr_ump_m2_note(0, GR_OP_NOTE_OFF, ch, note, 0, w);
      script_add(sc, t + 1 + rnd_next(&s) % 3000, w);
    } else if (kind < 70) {
      w[0] = gr_ump_m1(0, (uint8_t)(0xB0 | ch), (uint8_t)(r >> 24 & 127), (uint8_t)v);
      script_add(sc, t, w);
    } else if (kind < 75) {
      w[0] = gr_ump_m1(0, (uint8_t)(0xE0 | ch), (uint8_t)(v & 127), (uint8_t)(r >> 24 & 127));
      script_add(sc, t, w);
    } else if (kind < 80) {
      w[0] = gr_ump_m1(0, (uint8_t)(0xC0 | ch), (uint8_t)(v & 127), 0);
      script_add(sc, t, w);
    } else if (kind < 85) {
      w[0] = gr_ump_m1(0, (uint8_t)(0xD0 | ch), (uint8_t)(v & 127), 0);
      script_add(sc, t, w);
    } else if (kind < 90) {
      w[0] = gr_ump_m1(0, (uint8_t)(0xA0 | ch), (uint8_t)note, (uint8_t)v);
      script_add(sc, t, w);
    } else if (kind < 95) {
      w[0] = gr_ump_system(0, 0xF8, 0, 0);
      script_add(sc, t, w);
    } else if (kind < 97) {
      w[0] = gr_ump_system(0, (r & 1) ? 0xFA : 0xFC, 0, 0);
      script_add(sc, t, w);
    } else {  // a SysEx7 packet (two words), no MIDI 1.0 single form
      w[0] = 0x30000000u | (2u << 16) | 0x7D00u | (v & 127);
      w[1] = 0;
      script_add(sc, t, w);
    }
  }
  qsort(sc->ev, (size_t)sc->n, sizeof(ck_ev), ev_cmp);
}

// ---- feed nodes ----

typedef struct ck_feed {
  const ck_script *sc;
  int pos;
  uint32_t overflow;
  gr_events pushed;  // what it wrote this block: the subject must leave it as it was
} ck_feed;

static void feed_process(void *self, const gr_block *b, gr_io *io) {
  ck_feed *f = (ck_feed *)self;
  const uint64_t end = b->frame_pos + b->frames;
  while (f->pos < f->sc->n && f->sc->ev[f->pos].frame < end) {
    const ck_ev *e = &f->sc->ev[f->pos];
    if (!gr_events_push(io->ev_out[0], (uint32_t)(e->frame - b->frame_pos), e->ump)) {
      f->overflow++;
      break;
    }
    f->pos++;
  }
  memcpy(&f->pushed, io->ev_out[0], sizeof(gr_events));
}
static const gr_node_desc feed_desc = {"ck_feed", 0, 1, {0}, {GR_PORT_EVENTS}, feed_process};

typedef struct ck_afeed {
  float last[GR_MAX_FRAMES];
} ck_afeed;
static float signal_at(uint64_t abs) { return 0.5f * sinf((float)(abs % 48000u) * 0.0130899694f); }
static void afeed_process(void *self, const gr_block *b, gr_io *io) {
  ck_afeed *f = (ck_afeed *)self;
  for (uint32_t i = 0; i < b->frames; ++i) io->audio_out[0][i] = f->last[i] = signal_at(b->frame_pos + i);
}
static const gr_node_desc afeed_desc = {"ck_audio", 0, 1, {0}, {GR_PORT_AUDIO}, afeed_process};

// ---- one run ----

typedef struct ck_result {
  uint64_t hash[GR_CK_MAX_OUT];  // events (abs frame + words) and audio samples, in order
  uint32_t events[GR_CK_MAX_OUT];
  uint32_t outside, disorder, canary, nonfinite, inputs_changed, bad_ump;
  uint32_t dropped, feed_overflow, sounding, allocs;
  uint64_t last_on;  // absolute frame of the last note on out
  uint64_t ticks_total, blocks;
  uint32_t ticks_max;
  int build_rc;
} ck_result;

static gr_graph ck_g;
#define CK_CANARY 1234567.0f

static uint64_t fnv(uint64_t h, const void *p, size_t n) {
  const uint8_t *b = (const uint8_t *)p;
  for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
  return h;
}

static int desc_ok(const gr_node_desc *d, char *why, size_t len) {
  if (!d) return snprintf(why, len, "no descriptor"), 0;
  size_t nl = d->name ? strlen(d->name) : 0;
  if (nl < 1 || nl > 31) return snprintf(why, len, "name must be 1..31 characters (has %zu)", nl), 0;
  for (size_t i = 0; i < nl; ++i) {
    char c = d->name[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
          c == '-' || c == ':' || c == '.'))
      return snprintf(why, len, "name \"%s\": only letters, digits, _ - : .", d->name), 0;
  }
  if (!d->process) return snprintf(why, len, "%s: no process()", d->name), 0;
  if (d->n_in > GR_MAX_PORTS || d->n_out > GR_MAX_PORTS)
    return snprintf(why, len, "%s: %u in / %u out, at most %d each", d->name, d->n_in, d->n_out, GR_MAX_PORTS), 0;
  if (d->n_out == 0 && d->n_in == 0) return snprintf(why, len, "%s: no ports", d->name), 0;
  for (int p = 0; p < GR_MAX_PORTS; ++p) {
    int ti = d->in_type[p], to = d->out_type[p];
    if (p < d->n_in && ti != GR_PORT_EVENTS && ti != GR_PORT_AUDIO)
      return snprintf(why, len, "%s: input %d has no type", d->name, p), 0;
    if (p < d->n_out && to != GR_PORT_EVENTS && to != GR_PORT_AUDIO)
      return snprintf(why, len, "%s: output %d has no type", d->name, p), 0;
  }
  return 1;
}

typedef struct ck_plan {
  uint32_t size;  // 0 = random 1..GR_MAX_FRAMES
  uint32_t seed;
  uint32_t rate;
  int twiddle;
} ck_plan;

static void run_one(const gr_ck_subject *s, const ck_script *scripts, int n_in, uint64_t frames,
                    const ck_plan *plan, ck_result *res) {
  memset(res, 0, sizeof(*res));
  static ck_feed feeds[GR_MAX_PORTS];
  static ck_afeed afeeds[GR_MAX_PORTS];
  int feed_id[GR_MAX_PORTS], out_node[GR_CK_MAX_OUT], out_port[GR_CK_MAX_OUT], n_out = 0;
  void *mem = calloc(1, s->desc ? (s->self_size ? s->self_size : 1) : (s->mem_size ? s->mem_size : 1));
  gr_graph_init(&ck_g, plan->rate, 64);
  ck_g.ticks_per_us = 1000;
  int first_subject = 0;
  if (s->desc) {
    if (s->init && !s->init(mem)) {
      res->build_rc = -100;
      free(mem);
      return;
    }
    int ids[GR_MAX_PORTS];
    for (int p = 0; p < s->desc->n_in; ++p) {
      if (s->desc->in_type[p] == GR_PORT_EVENTS) {
        memset(&feeds[p], 0, sizeof(feeds[p]));
        feeds[p].sc = &scripts[p];
        ids[p] = gr_graph_add(&ck_g, &feed_desc, &feeds[p]);
      } else {
        ids[p] = gr_graph_add(&ck_g, &afeed_desc, &afeeds[p]);
      }
      feed_id[p] = ids[p];
    }
    first_subject = ck_g.n_nodes;
    int id = gr_graph_add(&ck_g, s->desc, mem);
    res->build_rc = id < 0 ? id : 0;
    for (int p = 0; p < s->desc->n_in && id >= 0; ++p) {
      int rc = gr_graph_connect(&ck_g, ids[p], 0, id, p);
      if (rc < 0) res->build_rc = rc;
    }
    for (int p = 0; p < s->desc->n_out && p < GR_CK_MAX_OUT; ++p) out_node[n_out] = id, out_port[n_out++] = p;
  } else {
    for (int i = 0; i < n_in; ++i) {
      memset(&feeds[i], 0, sizeof(feeds[i]));
      feeds[i].sc = &scripts[i];
      feed_id[i] = gr_graph_add(&ck_g, &feed_desc, &feeds[i]);
    }
    first_subject = ck_g.n_nodes;
    n_out = s->build(&ck_g, mem, feed_id, out_node, out_port);
    res->build_rc = n_out < 0 ? n_out : 0;
    if (n_out > GR_CK_MAX_OUT) n_out = GR_CK_MAX_OUT;
  }
  if (res->build_rc == 0) res->build_rc = gr_graph_compile(&ck_g);
  if (res->build_rc != 0) {
    if (s->fini) s->fini(mem);
    free(mem);
    return;
  }
  for (int o = 0; o < n_out; ++o) res->hash[o] = 1469598103934665603ull;
  // Notes sounding per output: [group*16+channel][note].
  static uint16_t sounding[GR_CK_MAX_OUT][256][128];
  memset(sounding, 0, sizeof(sounding));
  uint32_t rs = plan->seed ? plan->seed : 1;
  const uint64_t end = frames;
  int n_events_in = 0;
  for (int i = 0; i < GR_MAX_PORTS; ++i) n_events_in += (s->desc ? (i < s->desc->n_in) : (i < n_in)) ? 1 : 0;
  while (ck_g.block.frame_pos < end) {
    uint32_t f = plan->size ? plan->size : 1 + rnd_next(&rs) % GR_MAX_FRAMES;
    if (ck_g.block.frame_pos + f > end) f = (uint32_t)(end - ck_g.block.frame_pos);
    if (plan->twiddle && s->twiddle && rnd_next(&rs) % 37 == 0) s->twiddle(mem, rnd_next(&rs));
    // Canaries past this block's frames in every audio output.
    for (int o = 0; o < n_out; ++o) {
      float *a = ck_g.slot[out_node[o]].io.audio_out[out_port[o]];
      if (a)
        for (uint32_t i = f; i < GR_MAX_FRAMES; ++i) a[i] = CK_CANARY;
    }
    unsigned long a0 = gr_alloc_calls;
    gr_graph_run_frames(&ck_g, f);
    res->allocs += (uint32_t)(gr_alloc_calls - a0);
    const uint64_t pos = ck_g.block.frame_pos - f;
    // The subject's time (every node after the feeds).
    uint32_t bt = 0;
    for (int n = first_subject; n < ck_g.n_nodes; ++n) bt += ck_g.slot[n].t.last;
    res->ticks_total += bt;
    if (bt > res->ticks_max) res->ticks_max = bt;
    res->blocks++;
    // Inputs as the feeds left them?
    for (int i = 0; i < GR_MAX_PORTS; ++i) {
      int is_feed = s->desc ? (i < s->desc->n_in && s->desc->in_type[i] == GR_PORT_EVENTS) : (i < n_in);
      if (!is_feed) continue;
      const gr_events *now = ck_g.slot[feed_id[i]].io.ev_out[0];
      if (now->count != feeds[i].pushed.count ||
          memcmp(now->ev, feeds[i].pushed.ev, sizeof(gr_event) * now->count) != 0)
        res->inputs_changed++;
    }
    if (s->desc)
      for (int i = 0; i < s->desc->n_in; ++i)
        if (s->desc->in_type[i] == GR_PORT_AUDIO) {
          const float *now = ck_g.slot[feed_id[i]].io.audio_out[0];
          if (memcmp(now, afeeds[i].last, sizeof(float) * f) != 0) res->inputs_changed++;
        }
    for (int o = 0; o < n_out; ++o) {
      const gr_slot *sl = &ck_g.slot[out_node[o]];
      const gr_events *ev = sl->io.ev_out[out_port[o]];
      const float *au = sl->io.audio_out[out_port[o]];
      if (ev) {
        uint32_t last = 0;
        for (int i = 0; i < ev->count; ++i) {
          const gr_event *e = &ev->ev[i];
          if (e->frame >= f) res->outside++;
          if (e->frame < last) res->disorder++;
          last = e->frame;
          uint32_t ty = gr_ump_type(e->ump[0]);
          if (ty > 5) res->bad_ump++;
          uint64_t abs = pos + e->frame;
          res->hash[o] = fnv(res->hash[o], &abs, sizeof(abs));
          res->hash[o] = fnv(res->hash[o], e->ump, sizeof(uint32_t) * (size_t)gr_ump_words(e->ump[0]));
          res->events[o]++;
          uint32_t w0 = e->ump[0];
          if (gr_ump_is_cv(w0)) {
            uint32_t key = gr_ump_group(w0) * 16 + gr_ump_channel(w0), nt = gr_ump_index(w0) & 127;
            if (gr_ump_is_note_on(e->ump)) {
              sounding[o][key][nt]++;
              res->last_on = abs;
            }
            else if (gr_ump_is_note_off(e->ump) && sounding[o][key][nt])
              sounding[o][key][nt]--;
          }
        }
        res->dropped += ev->dropped;
      }
      if (au) {
        for (uint32_t i = 0; i < f; ++i)
          if (!isfinite(au[i])) res->nonfinite++;
        for (uint32_t i = f; i < GR_MAX_FRAMES; ++i)
          if (au[i] != CK_CANARY) {
            res->canary++;
            break;
          }
        res->hash[o] = fnv(res->hash[o], au, sizeof(float) * f);
        res->events[o] += f;
      }
    }
  }
  for (int i = 0; i < n_events_in && i < GR_MAX_PORTS; ++i) res->feed_overflow += feeds[i].overflow;
  for (int o = 0; o < n_out; ++o)
    for (int k = 0; k < 256; ++k)
      for (int nt = 0; nt < 128; ++nt) res->sounding += sounding[o][k][nt];
  if (s->fini) s->fini(mem);
  free(mem);
}

// ---- all the checks ----

static void checks(const gr_ck_subject *s, const gr_ck_options *o, gr_ck_report *r) {
  char why[160];
  // Format
  if (s->desc) {
    if (!desc_ok(s->desc, why, sizeof(why))) {
      add(r, "Format", 0, "%s", why);
      return;
    }
    add(r, "Format", 1, "%s: %u in, %u out, process() present", s->desc->name, s->desc->n_in, s->desc->n_out);
  } else if (!s->build || s->n_inputs < 0 || s->n_inputs > GR_MAX_PORTS) {
    add(r, "Format", 0, "an app needs build() and 0..%d inputs", GR_MAX_PORTS);
    return;
  }
  const int n_in = s->desc ? s->desc->n_in : s->n_inputs;
  const uint32_t sr = 48000;
  const uint64_t frames = (uint64_t)(o->seconds * sr) + 4000;
  ck_script scripts[GR_MAX_PORTS];
  memset(scripts, 0, sizeof(scripts));
  int total_in = 0;
  for (int i = 0; i < n_in; ++i) {
    script_make(&scripts[i], o->seed * 31u + (uint32_t)i, frames - 2000);
    if (s->flags & GR_CK_CLOCKED) {  // the song ends: without clock, a clocked node's time stands still
      uint32_t stop[4] = {gr_ump_system(0, 0xFC, 0, 0), 0, 0, 0};
      script_add(&scripts[i], frames - 1000, stop);
    }
    total_in += scripts[i].n;
  }
  static ck_result ref, res;
  ck_plan p64 = {64, 7, sr, 0};
  run_one(s, scripts, n_in, frames, &p64, &ref);
  if (ref.build_rc != 0) {
    add(r, "Format", 0, ref.build_rc == -100 ? "init() refused" : "the graph does not build (rc %d)", ref.build_rc);
    goto out;
  }
  if (!s->desc) {
    int bad = 0;
    for (int n = n_in; n < ck_g.n_nodes; ++n)
      if (!desc_ok(ck_g.slot[n].desc, why, sizeof(why))) {
        add(r, "Format", 0, "%s", why);
        bad = 1;
      }
    if (!bad) add(r, "Format", 1, "app: %d in, %d nodes, every descriptor valid", n_in, ck_g.n_nodes - n_in);
  }
  {
    // Blocks: the same script through every block plan.
    const ck_plan plans[] = {{1, 0, sr, 0},   {7, 0, sr, 0},   {48, 0, sr, 0},
                             {128, 0, sr, 0}, {256, 0, sr, 0}, {0, 11, sr, 0},
                             {0, 12, sr, 0},  {64, 0, 44100, 0}, {64, 0, 96000, 0}};
    const int n_plans = (int)(sizeof(plans) / sizeof(plans[0]));
    uint32_t outside = ref.outside, disorder = ref.disorder, canary = ref.canary, nonfinite = ref.nonfinite;
    uint32_t inputs = ref.inputs_changed, dropped = ref.dropped, allocs = ref.allocs, bad_ump = ref.bad_ump;
    uint32_t overflow = ref.feed_overflow;
    char differ[120] = "";
    int n_differ = 0;
    for (int k = 0; k < n_plans; ++k) {
      run_one(s, scripts, n_in, frames, &plans[k], &res);
      outside += res.outside, disorder += res.disorder, canary += res.canary, nonfinite += res.nonfinite;
      inputs += res.inputs_changed, dropped += res.dropped, allocs += res.allocs, bad_ump += res.bad_ump;
      overflow += res.feed_overflow;
      if (plans[k].rate != sr) continue;  // other rates: robustness only
      int same = 1;
      for (int q = 0; q < GR_CK_MAX_OUT; ++q) same &= res.hash[q] == ref.hash[q] && res.events[q] == ref.events[q];
      if (!same) {
        char one[16];
        snprintf(one, sizeof(one), plans[k].size ? "%u " : "random ", plans[k].size);
        strncat(differ, one, sizeof(differ) - strlen(differ) - 1);
        n_differ++;
      }
    }
    const char *plan_text = "blocks of 1, 7, 48, 64, 128, 256 and two random mixes at 48 kHz, 64 at 44.1 and 96 kHz";
    if (outside)
      add(r, "Blocks", 0, "%u events on a frame past their block's end (assumes a block size?)", outside);
    else
      add(r, "Blocks", 1, "every event inside its block: %s", plan_text);
    add(r, "Order", disorder == 0, disorder ? "%u events out of frame order" : "every output list in frame order", disorder);
    int has_audio_out = 0;
    if (s->desc)
      for (int q = 0; q < s->desc->n_out; ++q) has_audio_out |= s->desc->out_type[q] == GR_PORT_AUDIO;
    if (has_audio_out) {
      add(r, "Audio", canary == 0, canary ? "%u blocks wrote audio past the block's frames" : "no audio written past the block's frames", canary);
      add(r, "Audio", nonfinite == 0, nonfinite ? "%u samples NaN or infinite" : "no NaN, no infinity", nonfinite);
    } else {
      add(r, "Audio", -1, "no audio outputs");
    }
    add(r, "Inputs", inputs == 0, inputs ? "inputs changed in %u blocks (a node must never write its inputs)" : "inputs left as they were", inputs);
    add(r, "Events", bad_ump == 0 && dropped == 0,
        bad_ump ? "%u UMPs of an unknown type" : dropped ? "%u events dropped: an output list (%d a block) filled" : "%u in, valid UMPs out, none dropped",
        bad_ump ? bad_ump : dropped ? dropped : (uint32_t)total_in, GR_MAX_EVENTS);
    if (overflow) add(r, "Events", -1, "the checker's own feeds overflowed %u times (lower the density)", overflow);
    if (n_differ == 0)
      add(r, "Partition", 1, "same output at the same absolute frames for every block plan (%u events, ref. 64)", ref.events[0]);
    else if (s->flags & GR_CK_BLOCK_DEPENDENT)
      add(r, "Partition", -1, "differs at %s(declared block-dependent)", differ);
    else
      add(r, "Partition", 0, "output differs from 64-frame blocks at: %s(state kept per block, not per frame?)", differ);
    add(r, "Realtime", allocs == 0, allocs ? "%u allocator calls inside process()" : "no allocator calls counted inside process() (counts code built with the counting allocator: the Schwung modules, the broken nodes; the core's proof is graph_rt_symbols.sh)", allocs);
  }
  // Notes: balanced input, parameters changed between blocks, a second of quiet at the end.
  if (s->flags & GR_CK_NOTES) {
    ck_plan pn = {0, 21, sr, 1};
    run_one(s, scripts, n_in, frames + 48000, &pn, &res);
    const char *tw = s->twiddle ? " (parameters changed between blocks)" : "";
    const uint64_t quiet_from = frames + 48000 - 24000;  // the last half second of the quiet tail
    if (res.sounding && res.last_on >= quiet_from)
      add(r, "Notes", -1, "still playing half a second after its input ended (a sequencer?): stuck notes not judged");
    else if (res.sounding)
      add(r, "Notes", 0, "%u notes still sounding a second after the last note off%s", res.sounding, tw);
    else
      add(r, "Notes", 1, "every note on got its note off%s", tw);
  } else {
    add(r, "Notes", -1, "not a note path");
  }
  // Time, at 64-frame blocks (host ns: the P4's numbers come from the board).
  {
    double mean = ref.blocks ? (double)ref.ticks_total / (double)ref.blocks : 0;
    double budget = 64.0 * 1e9 / sr;
    add(r, "Time", mean <= budget * 0.25,
        "mean %.0f ns, max %u ns per 64-frame block on this host: %.3f%% of the %.0f ns budget%s", mean, ref.ticks_max,
        100.0 * mean / budget, budget, mean <= budget * 0.25 ? "" : " (over a quarter)");
  }
out:
  for (int i = 0; i < n_in; ++i) free(scripts[i].ev);
  // Known failures: reported, not counted.
  if (s->known)
    for (int i = 0; i < r->n; ++i) {
      size_t pl = strlen(r->line[i].part);
      if (r->line[i].ok == 0 && strncmp(s->known, r->line[i].part, pl) == 0 && s->known[pl] == ':') {
        r->line[i].ok = -2;
        r->fails--;
        add(r, "Known", -1, "%s", s->known);
      }
    }
}

int gr_ck_run(const gr_ck_subject *s, const gr_ck_options *o, gr_ck_report *r) {
  memset(r, 0, sizeof(*r));
  snprintf(r->name, sizeof(r->name), "%s", s->name ? s->name : "?");
  if (!o->isolate) {
    checks(s, o, r);
    return r->fails;
  }
  int fd[2];
  if (pipe(fd) != 0) {
    add(r, "Robustness", 0, "could not run (pipe)");
    return r->fails;
  }
  fflush(stdout);
  pid_t pid = fork();
  if (pid == 0) {
    close(fd[0]);
    alarm(o->timeout_s ? o->timeout_s : 60);
    gr_ck_report cr;
    memset(&cr, 0, sizeof(cr));
    snprintf(cr.name, sizeof(cr.name), "%s", r->name);
    checks(s, o, &cr);
    add(&cr, "Robustness", 1, "no crash, no hang (%.1f s of input per plan, 10 plans)", o->seconds);
    ssize_t w = write(fd[1], &cr, sizeof(cr));
    (void)w;
    _exit(0);
  }
  close(fd[1]);
  gr_ck_report cr;
  size_t got = 0;
  while (got < sizeof(cr)) {
    ssize_t n = read(fd[0], (char *)&cr + got, sizeof(cr) - got);
    if (n <= 0) break;
    got += (size_t)n;
  }
  close(fd[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  if (got == sizeof(cr)) {
    *r = cr;
  } else if (WIFSIGNALED(status)) {
    int sig = WTERMSIG(status);
    add(r, "Robustness", 0, sig == SIGALRM ? "hung: no result in %u s" : "crashed: signal %d (%s)",
        sig == SIGALRM ? o->timeout_s : (unsigned)sig, sig == SIGALRM ? "" : strsignal(sig));
  } else {
    add(r, "Robustness", 0, "the check stopped early (exit %d)", WEXITSTATUS(status));
  }
  return r->fails;
}
