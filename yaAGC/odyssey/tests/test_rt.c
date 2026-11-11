/*
 * ODYSSEY-AGC -- tests/test_rt.c : milestone 8 phase 1 (real-time audio core).
 *
 * Single-threaded exactness tests (ring, mixer, controls, underruns) plus
 * real multi-thread stress tests (producer / consumer / control threads).
 * Build with SAN=-fsanitize=thread (gcc) to check for data races.
 * The expected values come from straightforward double-precision formulas
 * written independently of odyssey_rt.c's integer code.
 */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "odyssey_rt.h"
#include "odyssey_backend.h"
#include <sys/stat.h>

static long checks, failures;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; \
    if (failures <= 25) { printf ("FAIL %s:%d: ", __FILE__, __LINE__); printf (__VA_ARGS__); printf ("\n"); } } } while (0)

static double
fl (double x) { return floor (x + 0.5); }

/* expected mixer output for one frame, per the specification */
static int
model (int a0, int a1, double g0, double g1, double master, int m0, int m1, int s0, int s1)
{
  int anysolo = s0 || s1;
  int au0 = !m0 && (!anysolo || s0), au1 = !m1 && (!anysolo || s1);
  /* gains are stored as Q15: model with the quantised values */
  double v0, v1, mix;
  g0 = ody_rt_gain_from_float (g0) / 32768.0;
  g1 = ody_rt_gain_from_float (g1) / 32768.0;
  master = ody_rt_gain_from_float (master) / 32768.0;
  v0 = fl (a0 * g0);
  v1 = fl (a1 * g1);
  if (v0 > 32767) v0 = 32767;
  if (v0 < -32767) v0 = -32767;
  if (v1 > 32767) v1 = 32767;
  if (v1 < -32767) v1 = -32767;
  mix = (au0 ? v0 : 0) + (au1 ? v1 : 0);
  mix = fl (mix * master);
  if (mix > 32767) mix = 32767;
  if (mix < -32767) mix = -32767;
  return (int) mix;
}

/* device rate == stream rate: exactly one stream sample per frame */
static void
mk (ody_rt_t *rt, uint32_t cap)
{
  if (ody_rt_init (rt, cap, 48000, 48000) != 0) { puts ("init failed"); exit (2); }
  ody_rt_set_active (rt, 0, 1);
  ody_rt_set_active (rt, 1, 1);
}

static void
test_ring_basics (void)
{
  ody_rt_t rt;
  ody_rt_ring_t *r;
  int i;
  int16_t out[40];
  ody_rt_chan_stats_t st;

  mk (&rt, 8);
  for (i = 0; i < 8; i++)
    CHECK (ody_rt_push (&rt, 0, (int16_t) (100 + i)) == 1, "push %d", i);
  CHECK (ody_rt_push (&rt, 0, 999) == 0, "9th push must drop (capacity 8)");
  CHECK (ody_rt_push (&rt, 0, 998) == 0, "10th push must drop");
  ody_rt_get_stats (&rt, 0, &st);
  CHECK (st.pushed == 8 && st.dropped == 2 && st.overrun_events == 1 && st.fill == 8,
         "ring full accounting: pushed %llu dropped %llu episodes %llu fill %u",
         (unsigned long long) st.pushed, (unsigned long long) st.dropped,
         (unsigned long long) st.overrun_events, st.fill);
  ody_rt_set_mute (&rt, 1, 1);
  ody_rt_render (&rt, out, 8, 1);
  for (i = 0; i < 8; i++)
    CHECK (out[i] == 100 + i, "FIFO order: out[%d]=%d", i, out[i]);
  ody_rt_get_stats (&rt, 0, &st);
  CHECK (st.consumed == 8 && st.fill == 0, "all consumed");
  CHECK (ody_rt_push (&rt, 0, 1) == 1, "space again");
  CHECK (ody_rt_push (&rt, 0, 2) == 1, "space again");
  for (i = 0; i < 6; i++) (void) ody_rt_push (&rt, 0, 3);
  CHECK (ody_rt_push (&rt, 0, 4) == 0, "full again");
  ody_rt_get_stats (&rt, 0, &st);
  CHECK (st.overrun_events == 2, "second full episode counted separately (%llu)",
         (unsigned long long) st.overrun_events);
  CHECK (ody_rt_push (&rt, 7, 1) == 0, "bad channel id rejected");

  /* index wrap: free-running 32-bit indices straddling 2^32 */
  r = &rt.chan[1].ring;
  atomic_store (&r->head, 0xFFFFFFF9u);
  atomic_store (&r->tail, 0xFFFFFFF9u);
  ody_rt_set_mute (&rt, 1, 0);
  ody_rt_set_mute (&rt, 0, 1);
  for (i = 0; i < 20; i++)
    {
      int k;
      for (k = 0; k < 5; k++)
        CHECK (ody_rt_push (&rt, 1, (int16_t) (i * 5 + k)) == 1, "wrap push");
      ody_rt_render (&rt, out, 5, 1);
      for (k = 0; k < 5; k++)
        CHECK (out[k] == i * 5 + k, "wrap order i=%d k=%d got %d", i, k, out[k]);
    }
  ody_rt_destroy (&rt);
}

static void
test_mixer (void)
{
  ody_rt_t rt;
  int16_t out[64];
  int a[64], b[64], i, c, case_n;
  static const struct { double g0, g1, master; int m0, m1, s0, s1; } cases[] = {
    { 1, 1, 1, 0, 0, 0, 0 },          /* both play                       */
    { 1, 1, 1, 0, 1, 0, 0 },          /* CMC alone (LGC muted)           */
    { 1, 1, 1, 1, 0, 0, 0 },          /* LGC alone (CMC muted)           */
    { 0.5, 0.25, 1, 0, 0, 0, 0 },     /* independent gains               */
    { 2.0, 0.0, 1, 0, 0, 0, 0 },      /* gain 0 silences only one        */
    { 1, 1, 0.5, 0, 0, 0, 0 },        /* master                          */
    { 1, 1, 1, 0, 0, 1, 0 },          /* solo CMC                        */
    { 1, 1, 1, 0, 0, 0, 1 },          /* solo LGC                        */
    { 1, 1, 1, 0, 0, 1, 1 },          /* both solo -> both play          */
    { 1, 1, 1, 1, 0, 1, 0 },          /* mute+solo same channel: silent  */
    { 1, 1, 1, 1, 0, 0, 1 },          /* CMC muted, LGC soloed           */
    { 1, 1, 1, 1, 1, 1, 1 },          /* everything muted+soloed: silent */
    { 1, 1, 1, 0, 1, 1, 0 },          /* LGC muted, CMC soloed           */
    { 1, 1, 1, 1, 0, 0, 0 },
    { 0.7, 1.3, 0.9, 0, 0, 0, 0 },
    { 4.0, 4.0, 4.0, 0, 0, 0, 0 },    /* huge: must clip, not wrap       */
  };
  for (case_n = 0; case_n < (int) (sizeof cases / sizeof cases[0]); case_n++)
    {
      for (c = 0; c < 2; c++)           /* c = 0 mono, 1 stereo */
        {
          mk (&rt, 128);
          for (i = 0; i < 64; i++)
            {
              a[i] = ((i * 997) % 65534) - 32767;   /* CMC pattern: full range */
              b[i] = ((i * 4001 + 12345) % 65534) - 32767;
              ody_rt_push (&rt, 0, (int16_t) a[i]);
              ody_rt_push (&rt, 1, (int16_t) b[i]);
            }
          ody_rt_set_gain_q15 (&rt, 0, ody_rt_gain_from_float (cases[case_n].g0));
          ody_rt_set_gain_q15 (&rt, 1, ody_rt_gain_from_float (cases[case_n].g1));
          ody_rt_set_master_q15 (&rt, ody_rt_gain_from_float (cases[case_n].master));
          ody_rt_set_mute (&rt, 0, cases[case_n].m0);
          ody_rt_set_mute (&rt, 1, cases[case_n].m1);
          ody_rt_set_solo (&rt, 0, cases[case_n].s0);
          ody_rt_set_solo (&rt, 1, cases[case_n].s1);
          {
            int16_t st[128];
            ody_rt_render (&rt, c ? st : out, 64, c ? 2 : 1);
            if (c)
              for (i = 0; i < 64; i++)
                {
                  CHECK (st[2 * i] == st[2 * i + 1], "stereo channels equal");
                  out[i] = st[2 * i];
                }
          }
          for (i = 0; i < 64; i++)
            {
              int want = model (a[i], b[i], cases[case_n].g0, cases[case_n].g1, cases[case_n].master,
                                cases[case_n].m0, cases[case_n].m1, cases[case_n].s0, cases[case_n].s1);
              CHECK (out[i] == want, "case %d ch%d frame %d: got %d want %d", case_n, c, i, out[i], want);
            }
          ody_rt_destroy (&rt);
        }
    }
}

static void
test_cross_contamination (void)
{
  ody_rt_t rt;
  int16_t out[200], out2[200];
  int i;
  /* CMC carries a ramp, LGC a distinct square.  With one muted the output must
   * be EXACTLY the other stream, sample for sample, whatever the muted one does. */
  mk (&rt, 256);
  for (i = 0; i < 200; i++)
    {
      ody_rt_push (&rt, 0, (int16_t) (i * 100 - 9000));
      ody_rt_push (&rt, 1, (int16_t) ((i / 7) % 2 ? 12345 : -12345));
    }
  ody_rt_set_mute (&rt, 1, 1);
  ody_rt_render (&rt, out, 200, 1);
  for (i = 0; i < 200; i++)
    CHECK (out[i] == i * 100 - 9000, "CMC-only exact at %d (got %d)", i, out[i]);
  ody_rt_destroy (&rt);

  mk (&rt, 256);
  for (i = 0; i < 200; i++)
    {
      ody_rt_push (&rt, 0, (int16_t) (i * 100 - 9000));
      ody_rt_push (&rt, 1, (int16_t) ((i / 7) % 2 ? 12345 : -12345));
    }
  ody_rt_set_mute (&rt, 0, 1);
  ody_rt_render (&rt, out2, 200, 1);
  for (i = 0; i < 200; i++)
    CHECK (out2[i] == ((i / 7) % 2 ? 12345 : -12345), "LGC-only exact at %d", i);
  ody_rt_destroy (&rt);

  /* sum of the two single-stream renders equals the mixed render */
  mk (&rt, 256);
  for (i = 0; i < 200; i++)
    {
      ody_rt_push (&rt, 0, (int16_t) (i * 100 - 9000));
      ody_rt_push (&rt, 1, (int16_t) ((i / 7) % 2 ? 12345 : -12345));
    }
  ody_rt_render (&rt, out, 200, 1);
  for (i = 0; i < 200; i++)
    CHECK (out[i] == (i * 100 - 9000) + ((i / 7) % 2 ? 12345 : -12345), "mix == sum at %d", i);
  ody_rt_destroy (&rt);
}

static void
test_zoh_and_clip (void)
{
  ody_rt_t rt;
  int16_t out[64];
  int i;
  ody_rt_chan_stats_t st;
  /* 4 device frames per stream sample: zero-order hold, first sample appears
   * after the first stream tick (frame 3 -> index 3). */
  CHECK (ody_rt_init (&rt, 64, 4000, 1000) == 0, "init");
  ody_rt_set_active (&rt, 0, 1);
  ody_rt_set_mute (&rt, 1, 1);
  for (i = 1; i <= 8; i++) ody_rt_push (&rt, 0, (int16_t) (i * 1000));
  ody_rt_render (&rt, out, 32, 1);
  for (i = 0; i < 32; i++)
    {
      int idx = (i + 1) / 4;                 /* samples consumed by frame i */
      int want = idx == 0 ? 0 : idx * 1000;
      CHECK (out[i] == want, "ZOH frame %d got %d want %d", i, out[i], want);
    }
  ody_rt_get_stats (&rt, 0, &st);
  CHECK (st.underrun_frames == 0, "no underrun while data present");
  ody_rt_destroy (&rt);

  /* clipping is saturating and counted; no wraparound */
  mk (&rt, 16);
  ody_rt_push (&rt, 0, 30000); ody_rt_push (&rt, 1, 30000);
  ody_rt_push (&rt, 0, -30000); ody_rt_push (&rt, 1, -30000);
  ody_rt_push (&rt, 0, 100); ody_rt_push (&rt, 1, 100);
  ody_rt_render (&rt, out, 3, 1);
  CHECK (out[0] == 32767 && out[1] == -32767 && out[2] == 200, "saturate: %d %d %d", out[0], out[1], out[2]);
  CHECK (ody_rt_clipped_frames (&rt) == 2, "clipped frames = 2 (%llu)", (unsigned long long) ody_rt_clipped_frames (&rt));
  ody_rt_destroy (&rt);
}

static void
test_underruns (void)
{
  ody_rt_t rt;
  int16_t out[256];
  ody_rt_chan_stats_t c, l;

  mk (&rt, 64);
  ody_rt_set_mute (&rt, 1, 1);
  /* 5 frames with 5 samples: no underrun */
  { int i; for (i = 0; i < 5; i++) ody_rt_push (&rt, 0, (int16_t) (10 + i)); }
  ody_rt_render (&rt, out, 5, 1);
  ody_rt_get_stats (&rt, 0, &c);
  CHECK (c.underrun_frames == 0 && c.underrun_events == 0, "no underrun yet");
  /* 7 starved frames: one episode, 7 frames, last value held */
  ody_rt_render (&rt, out, 7, 1);
  ody_rt_get_stats (&rt, 0, &c);
  CHECK (c.underrun_frames == 7 && c.underrun_events == 1, "starved 7 frames: %llu/%llu",
         (unsigned long long) c.underrun_frames, (unsigned long long) c.underrun_events);
  { int i; for (i = 0; i < 7; i++) CHECK (out[i] == 14, "hold last value, got %d", out[i]); }
  /* two samples arrive, then starvation again: 2 good frames then 2 starved frames */
  ody_rt_push (&rt, 0, 50); ody_rt_push (&rt, 0, 51);
  ody_rt_render (&rt, out, 4, 1);
  CHECK (out[0] == 50 && out[1] == 51 && out[2] == 51 && out[3] == 51, "recovery %d %d %d %d", out[0], out[1], out[2], out[3]);
  ody_rt_get_stats (&rt, 0, &c);
  CHECK (c.underrun_frames == 9 && c.underrun_events == 2, "second episode: %llu/%llu",
         (unsigned long long) c.underrun_frames, (unsigned long long) c.underrun_events);
  /* the muted LGC was active and empty throughout: it starves independently */
  ody_rt_get_stats (&rt, 1, &l);
  CHECK (l.underrun_frames == 16 && l.underrun_events == 1, "LGC counted independently: %llu/%llu",
         (unsigned long long) l.underrun_frames, (unsigned long long) l.underrun_events);
  ody_rt_destroy (&rt);

  /* after a long starvation the stream resumes at its own pace; it must not
   * fast-forward through queued samples to "catch up" with the missed ticks */
  CHECK (ody_rt_init (&rt, 64, 4000, 1000) == 0, "init");
  ody_rt_set_active (&rt, 0, 1);
  ody_rt_set_mute (&rt, 1, 1);
  ody_rt_render (&rt, out, 40, 1);               /* 10 stream ticks with nothing to play */
  { int i, last; for (i = 1; i <= 20; i++) ody_rt_push (&rt, 0, (int16_t) i);
    ody_rt_render (&rt, out, 16, 1);             /* 16 frames = 4 stream ticks */
    last = out[15];
    CHECK (last >= 1 && last <= 5, "no fast-forward after starvation: sample %d after 16 frames (<= 5 expected)", last); }
  ody_rt_destroy (&rt);

  /* an inactive computer never counts underruns */
  mk (&rt, 64);
  ody_rt_set_active (&rt, 1, 0);
  ody_rt_render (&rt, out, 100, 1);
  ody_rt_get_stats (&rt, 0, &c);
  ody_rt_get_stats (&rt, 1, &l);
  CHECK (c.underrun_frames == 100 && l.underrun_frames == 0 && l.underrun_events == 0,
         "inactive LGC: no underruns (%llu), CMC 100 (%llu)", (unsigned long long) l.underrun_frames,
         (unsigned long long) c.underrun_frames);
  ody_rt_destroy (&rt);
}

static void
test_prefill (void)
{
  ody_rt_t rt;
  int16_t out[64];
  int i;
  ody_rt_chan_stats_t st;
  mk (&rt, 64);
  ody_rt_set_mute (&rt, 1, 1);
  ody_rt_set_prefill (&rt, 0, 4);
  ody_rt_set_prefill (&rt, 1, 4);
  ody_rt_set_active (&rt, 0, 0);          /* re-activate so the new prefill applies */
  ody_rt_set_active (&rt, 0, 1);
  ody_rt_render (&rt, out, 3, 1);          /* empty ring: waiting, silent, not an underrun */
  ody_rt_get_stats (&rt, 0, &st);
  CHECK (st.underrun_frames == 0 && st.underrun_events == 0, "waiting for prefill is not an underrun");
  for (i = 0; i < 3; i++) CHECK (out[i] == 0, "silent while priming");
  for (i = 0; i < 3; i++) ody_rt_push (&rt, 0, (int16_t) (500 + i));
  ody_rt_render (&rt, out, 2, 1);          /* 3 < 4: still waiting */
  CHECK (out[0] == 0 && out[1] == 0, "3 of 4 queued: still silent");
  ody_rt_push (&rt, 0, 503);
  ody_rt_render (&rt, out, 5, 1);          /* 4 queued: starts with the first sample */
  CHECK (out[0] == 500 && out[1] == 501 && out[2] == 502 && out[3] == 503, "plays from the first queued sample");
  CHECK (out[4] == 503, "then starves and holds");
  ody_rt_get_stats (&rt, 0, &st);
  CHECK (st.underrun_frames == 1 && st.underrun_events == 1, "real underrun after priming counted (%llu)",
         (unsigned long long) st.underrun_frames);
  ody_rt_destroy (&rt);
}

/* ------------------------------------------------------------- threads */
typedef struct
{
  ody_rt_t *rt;
  int id;
  long n;                 /* samples to offer */
  int wait_when_full;     /* 1: retry until accepted, 0: drop */
  long offered;
} prod_t;

#define SEQ_MOD 30000

static void *
producer (void *p)
{
  prod_t *a = p;
  long i;
  for (i = 0; i < a->n; i++)
    {
      int16_t v = (int16_t) (i % SEQ_MOD);
      if (a->wait_when_full)
        while (!ody_rt_push (a->rt, a->id, v))
          sched_yield ();
      else
        (void) ody_rt_push (a->rt, a->id, v);
      if ((i & 255) == 0)
        sched_yield ();
    }
  a->offered = a->n;
  return NULL;
}

typedef struct { ody_rt_t *rt; _Atomic int stop; long bad_step; long gaps; long holds; long frames; long maxgap; } cons_t;

static void *
consumer (void *p)
{
  cons_t *c = p;
  int16_t buf[64];
  int prev = -1;
  while (!atomic_load (&c->stop))
    {
      uint32_t i;
      ody_rt_render (c->rt, buf, 64, 1);
      for (i = 0; i < 64; i++)
        {
          int v = buf[i];
          c->frames++;
          if (prev < 0)
            { if (v != 0 || 1) prev = v; continue; }
          if (v == prev) { c->holds++; continue; }
          {
            int d = (v - prev + SEQ_MOD) % SEQ_MOD;       /* forward distance */
            if (d == 0) c->bad_step++;
            else c->gaps += d - 1;
            if (d - 1 > c->maxgap) c->maxgap = d - 1;
            prev = v;
          }
        }
      if ((c->frames & 1023) == 0)
        sched_yield ();
    }
  return NULL;
}

static void
run_stream (const char *name, uint32_t cap, long n, int wait_when_full)
{
  ody_rt_t rt;
  pthread_t tp, tc;
  prod_t p;
  cons_t c;
  ody_rt_chan_stats_t st;
  int16_t tail[64];
  long remaining;

  mk (&rt, cap);
  ody_rt_set_mute (&rt, 1, 1);
  ody_rt_set_active (&rt, 1, 0);
  memset (&c, 0, sizeof c);
  c.rt = &rt;
  p.rt = &rt; p.id = 0; p.n = n; p.wait_when_full = wait_when_full; p.offered = 0;
  pthread_create (&tc, NULL, consumer, &c);
  pthread_create (&tp, NULL, producer, &p);
  pthread_join (tp, NULL);
  /* let the consumer drain what is left, then stop it */
  for (;;)
    {
      ody_rt_get_stats (&rt, 0, &st);
      if (st.fill == 0) break;
      sched_yield ();
    }
  atomic_store (&c.stop, 1);
  pthread_join (tc, NULL);
  ody_rt_get_stats (&rt, 0, &st);
  (void) tail;
  remaining = (long) st.fill;
  if (wait_when_full)   /* a refused attempt is retried: every sample ends up pushed exactly once */
    CHECK (st.pushed == (uint64_t) n, "%s: pushed %llu == offered %ld", name, (unsigned long long) st.pushed, n);
  else
    CHECK (st.pushed + st.dropped == (uint64_t) n, "%s: pushed %llu + dropped %llu == offered %ld", name,
           (unsigned long long) st.pushed, (unsigned long long) st.dropped, n);
  CHECK (st.consumed + (uint64_t) remaining == st.pushed, "%s: consumed %llu + left %ld == pushed %llu", name,
         (unsigned long long) st.consumed, remaining, (unsigned long long) st.pushed);
  CHECK (c.bad_step == 0, "%s: reordered/duplicated values: %ld", name, c.bad_step);
  if (wait_when_full)
    {
      CHECK (c.gaps == 0, "%s: gapless stream, gaps=%ld", name, c.gaps);
    }
  else
    {
      /* every skipped value in the consumed stream is a sample the producer dropped
       * (the very last dropped ones, after the final consumed value, are invisible) */
      CHECK ((uint64_t) c.gaps <= st.dropped, "%s: gaps %ld <= dropped %llu", name, c.gaps,
             (unsigned long long) st.dropped);
    }
  printf ("  %s: offered %ld pushed %llu dropped %llu overrun-episodes %llu consumed %llu holds %ld gaps %ld "
          "underrun-frames %llu\n", name, n, (unsigned long long) st.pushed, (unsigned long long) st.dropped,
          (unsigned long long) st.overrun_events, (unsigned long long) st.consumed, c.holds, c.gaps,
          (unsigned long long) st.underrun_frames);
  ody_rt_destroy (&rt);
}

/* control thread flips mute/solo/master while audio runs; every output frame must
 * be one of the values the control space allows. */
typedef struct { ody_rt_t *rt; _Atomic int stop; } ctl_t;

static void *
controller (void *p)
{
  ctl_t *c = p;
  unsigned x = 12345;
  while (!atomic_load (&c->stop))
    {
      x = x * 1664525u + 1013904223u;
      ody_rt_set_mute (c->rt, 0, (x >> 3) & 1);
      ody_rt_set_mute (c->rt, 1, (x >> 4) & 1);
      ody_rt_set_solo (c->rt, 0, (x >> 5) & 1);
      ody_rt_set_solo (c->rt, 1, (x >> 6) & 1);
      ody_rt_set_master_q15 (c->rt, (x >> 7) & 1 ? 32768 : 16384);
      ody_rt_set_gain_q15 (c->rt, 0, (x >> 8) & 1 ? 32768 : 16384);
      ody_rt_set_gain_q15 (c->rt, 1, (x >> 9) & 1 ? 32768 : 16384);
      sched_yield ();
    }
  return NULL;
}

static void
test_controls_while_running (void)
{
  ody_rt_t rt;
  ctl_t ctl;
  pthread_t tc;
  int16_t buf[128];
  long frames = 0, bad = 0;
  int i, ok[16384] = { 0 };
  /* CMC const 1000, LGC const 2000.  Allowed outputs: any sum of
   * {0, 500, 1000} + {0, 1000, 2000}, times {1, 0.5}.  Build the table. */
  static const int c0[] = { 0, 500, 1000 }, c1[] = { 0, 1000, 2000 };
  int a, b, m;
  for (a = 0; a < 3; a++) for (b = 0; b < 3; b++) for (m = 0; m < 2; m++)
    {
      int v = (int) fl ((c0[a] + c1[b]) * (m ? 0.5 : 1.0));
      if (v >= 0 && v < 16384) ok[v] = 1;
    }
  /* values after per-channel rounding at gain 0.5 are exact here */
  mk (&rt, 4096);
  ctl.rt = &rt;
  atomic_init (&ctl.stop, 0);
  pthread_create (&tc, NULL, controller, &ctl);
  for (i = 0; i < 4000; i++)
    {
      int k;
      for (k = 0; k < 128; k++)         /* steady producer (this thread is also the producer) */
        { ody_rt_push (&rt, 0, 1000); ody_rt_push (&rt, 1, 2000); }
      ody_rt_render (&rt, buf, 128, 1);
      for (k = 0; k < 128; k++)
        {
          frames++;
          if (buf[k] < 0 || buf[k] >= 16384 || !ok[buf[k]]) bad++;
        }
    }
  atomic_store (&ctl.stop, 1);
  pthread_join (tc, NULL);
  CHECK (bad == 0, "controls changed while running: %ld of %ld frames outside the allowed set", bad, frames);
  printf ("  controls-while-running: %ld frames, all within the allowed output set\n", frames);
  ody_rt_destroy (&rt);
}

static void
test_render_time (void)
{
  ody_rt_t rt;
  int16_t buf[1024];
  struct timespec t0, t1;
  uint64_t worst = 0, total = 0;
  int i, n = 20000;
  CHECK (ody_rt_init (&rt, 1024, 48000, 1000) == 0, "init");
  ody_rt_set_active (&rt, 0, 1);
  ody_rt_set_active (&rt, 1, 1);
  for (i = 0; i < n; i++)
    {
      uint64_t d;
      if (i % 4 == 0) { ody_rt_push (&rt, 0, (int16_t) i); ody_rt_push (&rt, 1, (int16_t) -i); }
      clock_gettime (CLOCK_MONOTONIC, &t0);
      ody_rt_render (&rt, buf, 512, 2);
      clock_gettime (CLOCK_MONOTONIC, &t1);
      d = (uint64_t) ((t1.tv_sec - t0.tv_sec) * 1000000000LL + (t1.tv_nsec - t0.tv_nsec));
      total += d;
      if (d > worst) worst = d;
    }
  printf ("  render(512 frames, stereo): mean %llu ns, worst %llu ns over %d calls (sandbox measurement; period budget 10.7 ms at 48 kHz)\n",
          (unsigned long long) (total / (uint64_t) n), (unsigned long long) worst, n);
  CHECK (worst < 5000000ull, "worst render %llu ns exceeds a gross 5 ms sanity bound", (unsigned long long) worst);
  ody_rt_destroy (&rt);
}

/* ---------------------------------------------------------- timed mode */
#define MCT 85333LL

static long long
Tof (long long f, long long lat) { return f * MCT / 48000 - lat; }

static void
mk_timed (ody_rt_t *rt, uint32_t cap, int64_t lat)
{
  if (ody_rt_init (rt, cap, 48000, 0) != 0) { puts ("init failed"); exit (2); }
  ody_rt_set_latency_cycles (rt, lat);
  ody_rt_set_active (rt, 0, 1);
  ody_rt_set_active (rt, 1, 1);
}

static void
test_timed (void)
{
  ody_rt_t rt;
  int16_t out[4096];
  int lat, i;
  long long f, W;
  ody_rt_chan_stats_t st;

  /* (a) every frame holds the newest sample stamped <= T, for several latencies and
   * both computers on the same clock; watermark far ahead (no starvation). */
  for (lat = 0; lat <= 2000; lat += 700)
    {
      long long cyc0[200], cyc1[200];
      mk_timed (&rt, 256, lat);
      for (i = 0; i < 200; i++)
        {
          cyc0[i] = 37 + (long long) i * 89 + (i % 5) * 3;     /* irregular spacing */
          cyc1[i] = 11 + (long long) i * 97 + (i % 7);
          ody_rt_push_timed (&rt, 0, (int16_t) (1000 + i * 10), (uint64_t) cyc0[i]);
          ody_rt_push_timed (&rt, 1, (int16_t) (-2000 - i * 7), (uint64_t) cyc1[i]);
        }
      ody_rt_publish_watermark (&rt, 1000000);
      ody_rt_render (&rt, out, 4096, 1);
      for (f = 0; f < 4096; f++)
        {
          long long T = Tof (f, lat);
          int a = 0, b = 0, want;
          for (i = 0; i < 200; i++)
            {
              if (T >= 0 && cyc0[i] <= T) a = 1000 + i * 10;
              if (T >= 0 && cyc1[i] <= T) b = -2000 - i * 7;
            }
          want = a + b;
          if (want > 32767) want = 32767;
          if (want < -32767) want = -32767;
          CHECK (out[f] == want, "timed lat=%d frame %lld: got %d want %d", lat, f, out[f], want);
        }
      ody_rt_get_stats (&rt, 0, &st);
      CHECK (st.underrun_frames == 0, "timed: no underrun when the watermark is ahead");
      ody_rt_destroy (&rt);
    }

  /* (b) underrun = AGC time beyond the watermark.  Exact frame accounting. */
  mk_timed (&rt, 256, 0);
  ody_rt_set_mute (&rt, 1, 1);
  for (i = 0; i < 100; i++)
    ody_rt_push_timed (&rt, 0, (int16_t) (i + 1), (uint64_t) (i * 100 + 50));   /* cycles 50..9950 */
  W = 3000;
  ody_rt_set_watermark (&rt, 0, (uint64_t) W);
  ody_rt_render (&rt, out, 2000, 1);                  /* T reaches 3555: past the watermark */
  {
    long long starved = 0, last_ok = -1;
    for (f = 0; f < 2000; f++)
      {
        if (Tof (f, 0) > W) starved++; else last_ok = f;
      }
    ody_rt_get_stats (&rt, 0, &st);
    CHECK (st.underrun_frames == (uint64_t) starved && st.underrun_events == 1,
           "timed starvation: %llu frames/%llu episodes, expected %lld/1", (unsigned long long) st.underrun_frames,
           (unsigned long long) st.underrun_events, starved);
    CHECK (starved > 100, "test really starves (%lld frames)", starved);
    CHECK (out[1999] == 30, "starved frames hold the newest sample the producer delivered (<= watermark 3000): %d", out[1999]);
    (void) last_ok;
  }
  /* producer catches up: playback resumes at the CORRECT AGC time (late samples are
   * skipped, not played late) and the episode ends. */
  ody_rt_set_watermark (&rt, 0, 20000);
  ody_rt_render (&rt, out, 100, 1);
  for (f = 0; f < 100; f++)
    {
      long long T = Tof (2000 + f, 0);
      int want = 0;
      for (i = 0; i < 100; i++) if (i * 100 + 50 <= T) want = i + 1;
      CHECK (out[f] == want, "resume frame %lld: got %d want %d (T=%lld)", 2000 + f, out[f], want, T);
    }
  ody_rt_get_stats (&rt, 0, &st);
  {
    long long starved = 0;
    for (f = 0; f < 2000; f++) if (Tof (f, 0) > W) starved++;
    CHECK (st.underrun_frames == (uint64_t) starved, "no further underrun after catching up");
  }
  ody_rt_destroy (&rt);

  /* (c) before AGC time 0 (latency) nothing plays and nothing counts */
  mk_timed (&rt, 64, ody_rt_latency_ms_to_cycles (10));
  ody_rt_render (&rt, out, 100, 1);
  ody_rt_get_stats (&rt, 0, &st);
  CHECK (st.underrun_frames == 0 && out[0] == 0 && out[99] == 0, "latency lead-in is silent and uncounted");
  CHECK (ody_rt_latency_ms_to_cycles (50) == 4266, "50 ms = 4266 cycles");
  ody_rt_destroy (&rt);

  /* (d) frames_ready matches a brute-force count */
  for (lat = 0; lat <= 4000; lat += 1333)
    {
      mk_timed (&rt, 64, lat);
      for (W = 0; W < 5000; W += 777)
        {
          long long n = 0;
          for (f = 0; f < 20000; f++) if (Tof (f, lat) <= W) n++;
          CHECK (ody_rt_frames_ready (&rt, (uint64_t) W) == (uint64_t) n, "frames_ready lat=%d W=%lld: %llu vs %lld", lat, W,
                 (unsigned long long) ody_rt_frames_ready (&rt, (uint64_t) W), n);
        }
      ody_rt_destroy (&rt);
    }
}

typedef struct { ody_rt_t *rt; long n; } tprod_t;

static void *
timed_producer (void *p)
{
  tprod_t *a = p;
  long i;
  for (i = 1; i <= a->n; i++)
    {
      while (!ody_rt_push_timed (a->rt, 0, (int16_t) i, (uint64_t) i * 89))
        sched_yield ();
      ody_rt_set_watermark (a->rt, 0, (uint64_t) i * 89);      /* after the push: release */
      if ((i & 63) == 0)
        sched_yield ();
    }
  ody_rt_set_watermark (a->rt, 0, (uint64_t) a->n * 89 + 100000);
  return NULL;
}

static void
test_timed_threads (void)
{
  ody_rt_t rt;
  pthread_t tp;
  tprod_t p;
  int16_t buf[64];
  long frames = 0, back = 0, mx = 0;
  int prev = 0;
  ody_rt_chan_stats_t st;

  mk_timed (&rt, 64, 0);
  ody_rt_set_active (&rt, 1, 0);
  p.rt = &rt; p.n = 30000;
  pthread_create (&tp, NULL, timed_producer, &p);
  while (mx < 30000 && frames < 40000000)
    {
      int k;
      ody_rt_render (&rt, buf, 64, 1);
      for (k = 0; k < 64; k++)
        {
          if (buf[k] < prev) back++;      /* the newest-sample-<=-T rule can never go backwards */
          prev = buf[k];
          if (buf[k] > mx) mx = buf[k];
        }
      frames += 64;
    }
  pthread_join (tp, NULL);
  ody_rt_get_stats (&rt, 0, &st);
  CHECK (back == 0, "timed threaded: %ld backward steps", back);
  CHECK (mx == 30000, "timed threaded: reached the last sample (%ld)", mx);
  CHECK (st.pushed == 30000 && st.consumed <= st.pushed, "timed threaded accounting: pushed %llu consumed %llu",
         (unsigned long long) st.pushed, (unsigned long long) st.consumed);
  printf ("  timed threaded: 30000 samples through a 64-slot ring, %ld frames, %llu underrun frames, %llu retries(full)\n",
          frames, (unsigned long long) st.underrun_frames, (unsigned long long) st.dropped);
  ody_rt_destroy (&rt);
}

static void
sleep_ms (long ms)
{
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (ms % 1000) * 1000000L;
  nanosleep (&ts, NULL);
}

typedef struct { ody_rt_t *rt; _Atomic int stop; long pushed; } feeder_t;

static void *
feeder (void *p)
{
  feeder_t *f = p;
  long i = 0;
  while (!atomic_load (&f->stop))
    {
      ody_rt_push (f->rt, 0, (int16_t) (1000 + (i++ % 100)));   /* 2000 samples/s */
      sleep_ms (0);                                              /* yield */
      { struct timespec ts = { 0, 500000L }; nanosleep (&ts, NULL); }
    }
  f->pushed = i;
  return NULL;
}

static void
test_paced_backend (void)
{
  const char *path = "build/testout/rt_paced.wav";
  ody_rt_t rt;
  ody_backend_t *be;
  ody_backend_cfg_t cfg;
  char err[160];
  ody_rt_chan_stats_t c, l;
  struct stat sb;
  feeder_t fd;
  pthread_t tf;

  /* (1) ACTUAL underruns: the callback runs on its own thread against a ring
   * nobody fills.  Every rendered frame is an underrun frame. */
  CHECK (ody_rt_init (&rt, 1024, 48000, 1000) == 0, "init");
  ody_rt_set_active (&rt, 0, 1);
  ody_rt_set_active (&rt, 1, 0);          /* LGC not playing: must not count */
  cfg.device = path; cfg.rate = 48000; cfg.period_frames = 256; cfg.channels = 2;
  be = ody_backend_create ("paced", &rt, &cfg, err, sizeof err);
  CHECK (be != NULL, "paced backend create: %s", err);
  if (be == NULL) { ody_rt_destroy (&rt); return; }
  CHECK (ody_backend_start (be) == 0, "start");
  sleep_ms (400);
  ody_backend_stop (be);
  ody_rt_get_stats (&rt, 0, &c);
  ody_rt_get_stats (&rt, 1, &l);
  {
    uint64_t periods = ody_backend_periods (be);
    printf ("  paced starved: %llu periods, rendered %llu frames, CMC underrun frames %llu events %llu, LGC %llu\n",
            (unsigned long long) periods, (unsigned long long) ody_rt_rendered_frames (&rt),
            (unsigned long long) c.underrun_frames, (unsigned long long) c.underrun_events,
            (unsigned long long) l.underrun_frames);
    CHECK (periods >= 60 && periods <= 90, "400 ms / 5.33 ms = ~75 periods expected when wall-clock paced (got %llu)",
           (unsigned long long) periods);
    CHECK (ody_rt_rendered_frames (&rt) == periods * 256, "rendered == periods x 256");
    /* no starvation is possible before the first stream tick: floor(2^32*1000/48000)*48 < 2^32,
     * so the first tick lands on frame index 48 and exactly rendered-48 frames starve. */
    CHECK (c.underrun_frames == ody_rt_rendered_frames (&rt) - 48, "every frame after the first tick starved (%llu of %llu)",
           (unsigned long long) c.underrun_frames, (unsigned long long) ody_rt_rendered_frames (&rt));
    CHECK (c.underrun_events == 1 && l.underrun_frames == 0 && l.underrun_events == 0,
           "one continuous episode; inactive LGC counts nothing");
  }
  ody_backend_destroy (be);
  CHECK (stat (path, &sb) == 0 && sb.st_size == 44 + (off_t) (ody_rt_rendered_frames (&rt) * 2 * 2),
         "WAV size = 44 + frames x 2ch x 2 bytes (%lld)", (long long) sb.st_size);
  ody_rt_destroy (&rt);

  /* (2) fed ahead of the clock: prefill 400 samples (0.4 s cushion at 1 kHz), then a
   * producer thread at 2000 samples/s.  The callback must not starve. */
  CHECK (ody_rt_init (&rt, 4096, 48000, 1000) == 0, "init");
  ody_rt_set_prefill (&rt, 0, 400);
  ody_rt_set_active (&rt, 0, 1);
  ody_rt_set_active (&rt, 1, 0);
  { int i; for (i = 0; i < 400; i++) ody_rt_push (&rt, 0, (int16_t) (1000 + i % 100)); }
  be = ody_backend_create ("paced", &rt, &cfg, err, sizeof err);
  CHECK (be != NULL, "paced backend create 2: %s", err);
  if (be == NULL) { ody_rt_destroy (&rt); return; }
  fd.rt = &rt; atomic_init (&fd.stop, 0); fd.pushed = 0;
  ody_backend_start (be);
  pthread_create (&tf, NULL, feeder, &fd);
  sleep_ms (600);
  atomic_store (&fd.stop, 1);
  pthread_join (tf, NULL);
  ody_backend_stop (be);
  ody_rt_get_stats (&rt, 0, &c);
  printf ("  paced fed: rendered %llu frames, consumed %llu, pushed %llu, dropped %llu, underrun frames %llu, "
          "max callback %llu ns\n", (unsigned long long) ody_rt_rendered_frames (&rt),
          (unsigned long long) c.consumed, (unsigned long long) c.pushed, (unsigned long long) c.dropped,
          (unsigned long long) c.underrun_frames, (unsigned long long) ody_backend_max_render_ns (be));
  CHECK (c.underrun_frames == 0, "no underrun with a 400-sample cushion (%llu)", (unsigned long long) c.underrun_frames);
  CHECK (c.consumed > 300 && c.consumed <= c.pushed, "consumed %llu <= pushed %llu", (unsigned long long) c.consumed,
         (unsigned long long) c.pushed);
  CHECK (c.dropped == 0, "no overruns");
  ody_backend_destroy (be);
  ody_rt_destroy (&rt);

  /* (3) unsupported / bad configurations fail cleanly */
  CHECK (ody_rt_init (&rt, 64, 48000, 1000) == 0, "init");
  cfg.rate = 44100;
  CHECK (ody_backend_create ("paced", &rt, &cfg, err, sizeof err) == NULL, "rate mismatch rejected");
  cfg.rate = 48000;
  CHECK (ody_backend_create ("alsa", &rt, &cfg, err, sizeof err) == NULL, "alsa unavailable in this build: %s", err);
  CHECK (ody_backend_create ("bogus", &rt, &cfg, err, sizeof err) == NULL, "unknown backend rejected");
  ody_rt_destroy (&rt);
}

int
main (void)
{
  alarm (280);                       /* a hung test (e.g. ring deadlock) must fail, not stall CI */
  printf ("test_rt: ring\n");            test_ring_basics ();
  printf ("test_rt: mixer exactness (16 cases x mono/stereo)\n"); test_mixer ();
  printf ("test_rt: cross-contamination\n"); test_cross_contamination ();
  printf ("test_rt: zero-order hold and clipping\n"); test_zoh_and_clip ();
  printf ("test_rt: underrun accounting\n"); test_underruns ();
  printf ("test_rt: prefill\n"); test_prefill ();
  printf ("test_rt: threaded sustained producer/consumer\n");
  run_stream ("blocking-producer ring 64 ", 64, 3000000, 1);
  run_stream ("blocking-producer ring 4096", 4096, 3000000, 1);
  run_stream ("dropping-producer ring 64  ", 64, 3000000, 0);
  printf ("test_rt: control changes while running\n"); test_controls_while_running ();
  printf ("test_rt: timed mode (cycle-stamped playback)\n"); test_timed (); test_timed_threads ();
  printf ("test_rt: paced device-emulation backend (real thread, wall clock)\n"); test_paced_backend ();
  printf ("test_rt: render time\n"); test_render_time ();
  printf ("test_rt: %ld checks, %ld failures\n", checks, failures);
  return failures ? 1 : 0;
}
