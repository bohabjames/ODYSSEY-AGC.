/*
 * ODYSSEY-AGC -- Filename: tests/test_rt_reset.c
 * M8 phase 1 review, item 4: what resetting ONE computer does to ITS audio.
 * usage: test_rt_reset ROMDIR     (needs PHASE-500, PHASE-1K)
 *
 * Contract under test (docs/AUDIO-ENGINE.md, "Reset semantics"):
 *   - the stream's cycle stamps live on the SCHEDULER timeline, not on the
 *     computer's own CycleCounter (which power-on reset restarts at 0), so they
 *     never run backwards: post-reset stamp = reset time + local cycle;
 *   - the reset itself emits ONE rest sample (PCM 0, seq == ODY_SEQ_REST) at the
 *     reset time: the AGC output channel is zero after power-on;
 *   - samples queued before the reset play out; nothing is flushed or dropped;
 *   - the other computer's audio, the mixer controls and the rt counters are
 *     untouched.
 * Independent oracle: out[f] = gain(PCM of the newest recorded sample whose
 * stamp is <= T(f)), T(f) = floor(f*85333/rate) - latency, 0 before the first.
 *
 * Each run renders ONE computer audible (the other muted) so that the audio of
 * the reset computer and of the untouched computer can be compared separately.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "odyssey_engine.h"
#include "odyssey_capture.h"
#include "odyssey_scheduler.h"
#include "odyssey_rt.h"

#ifndef ODY_SEQ_REST
#define ODY_SEQ_REST 0xFFFFFFFFu
#endif

static long checks, failures;
#define CHECK(cond)                                                          \
  do {                                                                       \
    checks++;                                                                \
    if (!(cond)) {                                                           \
      failures++;                                                            \
      fprintf (stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
    }                                                                        \
  } while (0)

#define RATE 48000u
#define SLICE 1000u
#define ROUNDS 256u                         /* about 3.0 AGC seconds        */
#define LAT_MS 50u
#define MAXS 20000
#define CAP_FRAMES ((uint64_t) ROUNDS * SLICE * RATE / 85333u + 8192)
static const uint32_t GAIN[2] = { 16384u, 49152u };   /* CMC 0.5, LGC 1.5 */

typedef struct
{
  uint64_t cycle[MAXS];
  int16_t pcm[MAXS];
  uint32_t seq[MAXS];
  size_t n;
  ody_rt_feed_t feed;
} rec_t;

typedef struct
{
  rec_t rec[2];                   /* [0]=CMC [1]=LGC samples as sunk            */
  int16_t *out;                   /* mono audio of the audible computer         */
  uint64_t frames;
  ody_rt_chan_stats_t st[2];
  uint32_t gain_after[2], mute_after[2];
} run_t;

static void
rec_sink (void *ctx, const ody_sample_t *s)
{
  rec_t *r = ctx;
  if (r->n < MAXS)
    {
      r->cycle[r->n] = s->cycle;
      r->seq[r->n] = s->seq;
      r->pcm[r->n] = ody_agc_to_pcm16 (s->word);
      r->n++;
    }
  ody_rt_sink (&r->feed, s);
}

static const char *romdir;

/* resets: rounds after which the CMC is reset (0-terminated); audible = 0/1 */
static int
run (run_t *r, const unsigned *resets, int audible)
{
  static odyssey_computer_t cmc, lgc;
  odyssey_scheduler_t s;
  ody_rt_t rt;
  char p1[512], p2[512];
  unsigned round, q;
  int16_t *buf = calloc (4096, sizeof (int16_t));

  memset (r, 0, sizeof *r);
  r->out = calloc (CAP_FRAMES, sizeof (int16_t));
  snprintf (p1, sizeof p1, "%s/PHASE-500.bin", romdir);
  snprintf (p2, sizeof p2, "%s/PHASE-1K.bin", romdir);
  if (odyssey_computer_init (&cmc, ODY_COMPUTER_CMC, p1) != 0 ||
      odyssey_computer_init (&lgc, ODY_COMPUTER_LGC, p2) != 0 ||
      odyssey_scheduler_init (&s, &cmc, &lgc, SLICE) != 0 ||
      ody_rt_init (&rt, 1u << 16, RATE, 0) != 0)
    return -1;
  ody_rt_set_latency_cycles (&rt, ody_rt_latency_ms_to_cycles (LAT_MS));
  for (q = 0; q < 2; q++)
    {
      r->rec[q].feed.rt = &rt;
      r->rec[q].feed.id = (int) q;
      odyssey_scheduler_set_sink (&s, (int) q, rec_sink, &r->rec[q]);
      ody_rt_set_active (&rt, (int) q, 1);
      ody_rt_set_gain_q15 (&rt, (int) q, GAIN[q]);
      ody_rt_set_mute (&rt, (int) q, (int) q != audible);
    }
  for (round = 1; round <= ROUNDS; round++)
    {
      uint64_t due;
      odyssey_scheduler_round (&s, SLICE);
      ody_rt_publish_watermark (&rt, s.cycles);
      for (q = 0; resets && resets[q]; q++)
        if (resets[q] == round)
          CHECK (odyssey_scheduler_reset_computer (&s, ODY_COMPUTER_CMC) == 0);
      due = ody_rt_frames_ready (&rt, s.cycles);
      while (r->frames < due)
        {
          uint64_t left = due - r->frames;
          uint32_t n = left > 4096 ? 4096u : (uint32_t) left;
          ody_rt_render (&rt, buf, n, 1);
          memcpy (r->out + r->frames, buf, n * sizeof (int16_t));
          r->frames += n;
        }
    }
  for (q = 0; q < 2; q++)
    {
      ody_rt_chan_stats_t *st = &r->st[q];
      ody_rt_get_stats (&rt, (int) q, st);
      r->gain_after[q] = atomic_load (&rt.chan[q].gain_q15);
      r->mute_after[q] = atomic_load (&rt.chan[q].mute);
    }
  free (buf);
  ody_rt_destroy (&rt);
  odyssey_computer_destroy (&cmc);
  odyssey_computer_destroy (&lgc);
  return 0;
}

/* Independent oracle for one computer's audio from its recorded stamps. */
static void
oracle (const rec_t *rec, uint32_t gain_q15, uint64_t frames, int16_t *want)
{
  size_t k = 0;
  int16_t cur = 0;
  uint64_t f;
  int64_t lat = ody_rt_latency_ms_to_cycles (LAT_MS);
  for (f = 0; f < frames; f++)
    {
      int64_t T = (int64_t) (f * 85333ull / RATE) - lat, v;
      if (T >= 0)
        while (k < rec->n && rec->cycle[k] <= (uint64_t) T)
          cur = rec->pcm[k++];
      v = ((int64_t) cur * gain_q15 + 16384) >> 15;
      want[f] = (int16_t) (v > 32767 ? 32767 : v < -32767 ? -32767 : v);
    }
}

static size_t
count_rest (const rec_t *r)
{
  size_t i, n = 0;
  for (i = 0; i < r->n; i++)
    n += r->seq[i] == ODY_SEQ_REST;
  return n;
}

static void
free_run (run_t *r)
{
  free (r->out);
}

int
main (int argc, char **argv)
{
  static run_t base_c, base_l, one_c, one_l, two_c;
  size_t i, post, first_real;
  const uint64_t R = 100ull * SLICE, R2 = 101ull * SLICE;
  int16_t *want;
  const unsigned r1[] = { 100, 0 }, r2[] = { 100, 101, 0 };

  if (argc < 2)
    {
      fprintf (stderr, "usage: %s ROMDIR\n", argv[0]);
      return 2;
    }
  romdir = argv[1];
  if (run (&base_c, NULL, 0) || run (&base_l, NULL, 1) || run (&one_c, r1, 0) ||
      run (&one_l, r1, 1) || run (&two_c, r2, 0))
    {
      fprintf (stderr, "setup failed\n");
      return 2;
    }

  /* ---- baseline sanity: no marker, stamps monotonic, nothing lost ---- */
  CHECK (base_c.rec[0].n > 1000 && base_c.rec[1].n > 1000);
  for (i = 1; i < base_c.rec[0].n; i++)
    CHECK (base_c.rec[0].cycle[i] >= base_c.rec[0].cycle[i - 1]);
  CHECK (count_rest (&base_c.rec[0]) == 0 && count_rest (&base_c.rec[1]) == 0);

  /* ---- single reset of the CMC ---- */
  for (i = 1; i < one_c.rec[0].n; i++)
    CHECK (one_c.rec[0].cycle[i] >= one_c.rec[0].cycle[i - 1]);   /* never backwards */
  CHECK (count_rest (&one_c.rec[0]) == 1);
  CHECK (count_rest (&one_c.rec[1]) == 0);
  for (first_real = 0; first_real < one_c.rec[0].n; first_real++)
    if (one_c.rec[0].seq[first_real] == ODY_SEQ_REST)
      {
        CHECK (one_c.rec[0].cycle[first_real] == R);
        CHECK (one_c.rec[0].pcm[first_real] == 0);
        break;
      }
  first_real++;                                  /* first real post-reset sample */

  /* post-reset stream == the baseline's stream from power-on, shifted by R */
  for (i = first_real, post = 0; i < one_c.rec[0].n && post < base_c.rec[0].n; i++, post++)
    {
      CHECK (one_c.rec[0].cycle[i] == base_c.rec[0].cycle[post] + R);
      CHECK (one_c.rec[0].pcm[i] == base_c.rec[0].pcm[post]);
      CHECK (one_c.rec[0].seq[i] == base_c.rec[0].seq[post]);   /* seq restarted at 0 */
    }
  CHECK (post > 500);

  /* the LGC is unaffected: same samples, same stamps, same audio */
  CHECK (one_l.rec[1].n == base_l.rec[1].n);
  for (i = 0; i < base_l.rec[1].n && i < one_l.rec[1].n; i++)
    CHECK (one_l.rec[1].cycle[i] == base_l.rec[1].cycle[i] &&
           one_l.rec[1].pcm[i] == base_l.rec[1].pcm[i]);
  CHECK (one_l.frames == base_l.frames);
  CHECK (memcmp (one_l.out, base_l.out, base_l.frames * sizeof (int16_t)) == 0);

  /* the CMC's audio equals the oracle over the WHOLE run, reset included */
  want = calloc (CAP_FRAMES, sizeof (int16_t));
  oracle (&one_c.rec[0], GAIN[0], one_c.frames, want);
  CHECK (memcmp (want, one_c.out, one_c.frames * sizeof (int16_t)) == 0);
  /* before the reset the audio equals the baseline's: queued samples played out */
  {
    uint64_t pre = R * RATE / 85333ull - 2400;
    CHECK (memcmp (base_c.out, one_c.out, pre * sizeof (int16_t)) == 0);
  }

  /* nothing dropped or starved; every pushed sample consumed or still queued */
  for (i = 0; i < 2; i++)
    {
      CHECK (one_c.st[i].dropped == 0 && one_c.st[i].underrun_frames == 0);
      CHECK (one_c.st[i].pushed == one_c.st[i].consumed + one_c.st[i].fill);
    }
  /* the reset does not touch the mixer or the counters' meaning */
  CHECK (one_c.gain_after[0] == GAIN[0] && one_c.gain_after[1] == GAIN[1]);
  CHECK (one_c.mute_after[0] == 0 && one_c.mute_after[1] == 1);
  CHECK (one_c.st[1].pushed == base_c.st[1].pushed);

  /* ---- two resets in consecutive rounds ---- */
  for (i = 1; i < two_c.rec[0].n; i++)
    CHECK (two_c.rec[0].cycle[i] >= two_c.rec[0].cycle[i - 1]);
  CHECK (count_rest (&two_c.rec[0]) == 2);
  {
    size_t nr = 0;
    for (i = 0; i < two_c.rec[0].n; i++)
      if (two_c.rec[0].seq[i] == ODY_SEQ_REST)
        CHECK (two_c.rec[0].cycle[i] == (++nr == 1 ? R : R2));
  }
  oracle (&two_c.rec[0], GAIN[0], two_c.frames, want);
  CHECK (memcmp (want, two_c.out, two_c.frames * sizeof (int16_t)) == 0);
  CHECK (two_c.st[0].dropped == 0 && two_c.st[0].underrun_frames == 0);
  free (want);

  free_run (&base_c); free_run (&base_l); free_run (&one_c); free_run (&one_l); free_run (&two_c);
  printf ("test_rt_reset: %ld checks, %ld failures\n", checks, failures);
  return failures ? 1 : 0;
}
