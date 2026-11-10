/*
 * ODYSSEY-AGC -- Filename: tests/test_sched.c
 * Milestone 5 tests: two independent computers under the deterministic
 * scheduler.  usage: test_sched ROMDIR   (needs PHASE-500, PHASE-1K, SEQTEST)
 *
 * "Fingerprint" of a computer = FNV-1a over its CSV sample stream, plus its
 * cycle count, sample count and a hash of its full erasable memory.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "odyssey_engine.h"
#include "odyssey_capture.h"
#include "odyssey_scheduler.h"

static int failures = 0, checks = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    checks++;                                                                \
    if (!(cond)) {                                                           \
      failures++;                                                            \
      fprintf (stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
    }                                                                        \
  } while (0)

typedef struct
{
  uint64_t stream_hash, erasable_hash, cycles, samples, seq_gaps;
} fp_t;

static uint64_t
fnv (uint64_t h, const void *p, size_t n)
{
  const unsigned char *b = (const unsigned char *) p;
  while (n--)
    {
      h ^= *b++;
      h *= 1099511628211ULL;
    }
  return h;
}

static uint64_t
hash_file_from (FILE *f, long from)
{
  uint64_t h = 1469598103934665603ULL;
  unsigned char buf[4096];
  size_t n;
  fflush (f);
  fseek (f, from, SEEK_SET);
  while ((n = fread (buf, 1, sizeof buf, f)) > 0)
    h = fnv (h, buf, n);
  return h;
}

static void
fingerprint (fp_t *fp, FILE *csv, long csv_from, const odyssey_computer_t *c,
             const ody_capture_t *cap)
{
  fp->stream_hash = hash_file_from (csv, csv_from);
  fp->erasable_hash = fnv (1469598103934665603ULL, c->agc.Erasable,
                           sizeof c->agc.Erasable);
  fp->cycles = c->cycles_executed;
  fp->samples = cap->samples;
  fp->seq_gaps = cap->seq_gaps;
}

static int
fp_equal (const fp_t *a, const fp_t *b)
{
  return a->stream_hash == b->stream_hash && a->erasable_hash == b->erasable_hash &&
         a->cycles == b->cycles && a->samples == b->samples &&
         a->seq_gaps == b->seq_gaps;
}

/* One computer, run alone, in slices of `slice`. */
static int
run_isolated (const char *rom, int id, uint64_t total, uint32_t slice, fp_t *out)
{
  odyssey_computer_t *c = (odyssey_computer_t *) calloc (1, sizeof *c);
  ody_capture_t cap;
  FILE *csv = tmpfile ();
  uint64_t done = 0;

  if (c == NULL || csv == NULL || odyssey_computer_init (c, id, rom) != 0)
    return -1;
  ody_capture_init (&cap, csv);
  while (done < total)
    {
      uint32_t n = (total - done < slice) ? (uint32_t) (total - done) : slice;
      done += odyssey_computer_step (c, n);
      ody_capture_drain (&cap, c);
    }
  fingerprint (out, csv, 0, c, &cap);
  fclose (csv);
  odyssey_computer_destroy (c);
  free (c);
  return 0;
}

typedef struct
{
  odyssey_computer_t *cmc, *lgc;
  odyssey_scheduler_t s;
  FILE *fc, *fl;
} dual_t;

static int
dual_open (dual_t *d, const char *cmc_rom, const char *lgc_rom, uint32_t slice)
{
  memset (d, 0, sizeof *d);
  d->cmc = (odyssey_computer_t *) calloc (1, sizeof *d->cmc);
  d->lgc = (odyssey_computer_t *) calloc (1, sizeof *d->lgc);
  d->fc = tmpfile ();
  d->fl = tmpfile ();
  if (!d->cmc || !d->lgc || !d->fc || !d->fl)
    return -1;
  if (odyssey_computer_init (d->cmc, ODY_COMPUTER_CMC, cmc_rom) != 0 ||
      odyssey_computer_init (d->lgc, ODY_COMPUTER_LGC, lgc_rom) != 0)
    return -2;
  if (odyssey_scheduler_init (&d->s, d->cmc, d->lgc, slice) != 0)
    return -3;
  odyssey_scheduler_set_csv (&d->s, ODY_COMPUTER_CMC, d->fc);
  odyssey_scheduler_set_csv (&d->s, ODY_COMPUTER_LGC, d->fl);
  return 0;
}

static void
dual_close (dual_t *d)
{
  if (d->fc) fclose (d->fc);
  if (d->fl) fclose (d->fl);
  if (d->cmc) { odyssey_computer_destroy (d->cmc); free (d->cmc); }
  if (d->lgc) { odyssey_computer_destroy (d->lgc); free (d->lgc); }
}

int
main (int argc, char **argv)
{
  char r500[600], r1k[600], rseq[600];
  const uint32_t slices[] = { 1, 7, 1000, 4096 };
  const uint64_t TOTAL = 200000;
  size_t i;
  fp_t iso_c, iso_l, f;
  dual_t d;
  odyssey_computer_t *x, *y;
  ody_sample_t smp;

  if (argc < 2)
    {
      fprintf (stderr, "usage: %s ROMDIR\n", argv[0]);
      return 2;
    }
  snprintf (r500, sizeof r500, "%s/PHASE-500.bin", argv[1]);
  snprintf (r1k, sizeof r1k, "%s/PHASE-1K.bin", argv[1]);
  snprintf (rseq, sizeof rseq, "%s/SEQTEST.bin", argv[1]);

  /* ---- scheduler argument validation ---------------------------------- */
  {
    odyssey_scheduler_t s;
    odyssey_computer_t a, b;
    memset (&a, 0, sizeof a);
    memset (&b, 0, sizeof b);
    CHECK (odyssey_scheduler_init (&s, NULL, &b, 10) < 0);
    CHECK (odyssey_scheduler_init (&s, &a, &a, 10) < 0);
    CHECK (odyssey_scheduler_init (&s, &a, &b, 10) < 0);   /* not initialised */
  }

  /* ---- 1/2/3/4/5: independent init, different ROMs, stepping one ------- */
  CHECK (dual_open (&d, r500, r1k, 1000) == 0);
  x = d.cmc;
  y = d.lgc;
  CHECK (&x->agc != &y->agc);
  CHECK (x->agc.agc_clientdata == x && y->agc.agc_clientdata == y);
  CHECK (memcmp (x->agc.Fixed, y->agc.Fixed, sizeof x->agc.Fixed) != 0);
  CHECK (strcmp (x->rom_path, y->rom_path) != 0);
  CHECK (x->id == ODY_COMPUTER_CMC && y->id == ODY_COMPUTER_LGC);

  /* stepping CMC alone does not advance LGC */
  CHECK (odyssey_computer_step (x, 5000) == 5000);
  CHECK (x->cycles_executed == 5000 && x->agc.CycleCounter > 0);
  CHECK (y->cycles_executed == 0 && y->agc.CycleCounter == 0);
  CHECK (y->sample_seq == 0 && ody_queue_size (&y->samples) == 0);
  CHECK (x->sample_seq > 0 && ody_queue_size (&x->samples) == x->sample_seq);
  /* samples are tagged with, and queued only in, their own computer */
  CHECK (ody_queue_pop (&x->samples, &smp) && smp.computer == ODY_COMPUTER_CMC &&
         smp.seq == 0);
  {
    uint64_t xcyc = x->agc.CycleCounter;
    uint32_t xseq = x->sample_seq;
    /* stepping LGC alone does not advance CMC */
    CHECK (odyssey_computer_step (y, 5000) == 5000);
    CHECK (x->agc.CycleCounter == xcyc && x->cycles_executed == 5000 &&
           x->sample_seq == xseq);
    CHECK (y->sample_seq > 0);
    CHECK (ody_queue_pop (&y->samples, &smp) && smp.computer == ODY_COMPUTER_LGC &&
           smp.seq == 0);
  }
  dual_close (&d);

  /* ---- the two ROMs really behave differently (non-vacuous) ------------ */
  CHECK (run_isolated (r500, ODY_COMPUTER_CMC, TOTAL, 1000, &iso_c) == 0);
  CHECK (run_isolated (r1k, ODY_COMPUTER_LGC, TOTAL, 1000, &iso_l) == 0);
  CHECK (iso_c.stream_hash != iso_l.stream_hash);
  CHECK (iso_c.samples > 100 && iso_l.samples > iso_c.samples);
  CHECK (iso_c.seq_gaps == 0 && iso_l.seq_gaps == 0);

  /* ---- dual run == isolated run, at several interleave granularities ---- */
  for (i = 0; i < sizeof slices / sizeof slices[0]; i++)
    {
      fp_t dc, dl;
      CHECK (dual_open (&d, r500, r1k, slices[i]) == 0);
      odyssey_scheduler_run (&d.s, TOTAL);
      fingerprint (&dc, d.fc, 0, d.cmc, &d.s.capture[ODY_COMPUTER_CMC]);
      fingerprint (&dl, d.fl, 0, d.lgc, &d.s.capture[ODY_COMPUTER_LGC]);
      CHECK (fp_equal (&dc, &iso_c));
      CHECK (fp_equal (&dl, &iso_l));
      CHECK (d.cmc->cycles_executed == TOTAL && d.lgc->cycles_executed == TOTAL);
      CHECK (d.s.rounds == (TOTAL + slices[i] - 1) / slices[i]);
      dual_close (&d);
    }

  /* ---- isolated results themselves do not depend on slice size ---------- */
  CHECK (run_isolated (r500, ODY_COMPUTER_CMC, TOTAL, 7, &f) == 0 && fp_equal (&f, &iso_c));

  /* ---- same ROM in both seats: both run identically and independently --- */
  {
    fp_t a, b;
    CHECK (dual_open (&d, r1k, r1k, 7) == 0);
    odyssey_scheduler_run (&d.s, TOTAL);
    fingerprint (&a, d.fc, 0, d.cmc, &d.s.capture[ODY_COMPUTER_CMC]);
    fingerprint (&b, d.fl, 0, d.lgc, &d.s.capture[ODY_COMPUTER_LGC]);
    /* same program, same cycles => identical streams ... */
    CHECK (a.stream_hash == b.stream_hash && a.erasable_hash == b.erasable_hash);
    /* ... but they are two separate memories */
    CHECK (&d.cmc->agc.Erasable[0][0] != &d.lgc->agc.Erasable[0][0]);
    /* mutate one memory; the other must not see it */
    d.cmc->agc.Erasable[3][5] ^= 0x1555;
    CHECK (d.cmc->agc.Erasable[3][5] != d.lgc->agc.Erasable[3][5]);
    d.cmc->agc.Erasable[3][5] ^= 0x1555;
    CHECK (fp_equal (&a, &iso_l));
    dual_close (&d);
  }

  /* ---- reset of one computer leaves the other unchanged ----------------- */
  {
    fp_t before_b, after_b, dc_tail, dl_full, iso_c_half, iso_l_full;
    long off;
    odyssey_computer_t *snap = (odyssey_computer_t *) calloc (1, sizeof *snap);

    CHECK (run_isolated (r500, ODY_COMPUTER_CMC, TOTAL / 2, 1000, &iso_c_half) == 0);
    CHECK (run_isolated (r1k, ODY_COMPUTER_LGC, TOTAL, 1000, &iso_l_full) == 0);

    CHECK (dual_open (&d, r500, r1k, 7) == 0);
    odyssey_scheduler_run (&d.s, TOTAL / 2);
    fingerprint (&before_b, d.fl, 0, d.lgc, &d.s.capture[ODY_COMPUTER_LGC]);
    memcpy (&snap->agc, &d.lgc->agc, sizeof snap->agc);
    fflush (d.fc);
    off = ftell (d.fc);                       /* CMC stream position at reset */
    CHECK (odyssey_scheduler_reset_computer (&d.s, ODY_COMPUTER_CMC) == 0);
    /* LGC bit-for-bit untouched by the CMC reset */
    CHECK (memcmp (&snap->agc, &d.lgc->agc, sizeof snap->agc) == 0);
    fingerprint (&after_b, d.fl, 0, d.lgc, &d.s.capture[ODY_COMPUTER_LGC]);
    CHECK (fp_equal (&before_b, &after_b));
    CHECK (d.cmc->cycles_executed == 0 && d.cmc->sample_seq == 0 &&
           d.s.capture[ODY_COMPUTER_CMC].samples == 0);
    CHECK (d.cmc->agc.agc_clientdata == d.cmc);

    /* keep running: LGC continues exactly as if nothing happened;
     * CMC restarts and reproduces a fresh half-length run */
    odyssey_scheduler_run (&d.s, TOTAL / 2);
    fingerprint (&dl_full, d.fl, 0, d.lgc, &d.s.capture[ODY_COMPUTER_LGC]);
    CHECK (fp_equal (&dl_full, &iso_l_full));
    fingerprint (&dc_tail, d.fc, off, d.cmc, &d.s.capture[ODY_COMPUTER_CMC]);
    CHECK (fp_equal (&dc_tail, &iso_c_half));
    free (snap);

    /* ---- destroying one computer leaves the other intact -------------- */
    {
      uint64_t lc = d.lgc->agc.CycleCounter;
      odyssey_computer_destroy (d.cmc);
      CHECK (d.cmc->initialized == 0);
      CHECK (d.lgc->initialized == 1 && d.lgc->agc.CycleCounter == lc);
      CHECK (odyssey_computer_step (d.lgc, 1000) == 1000);
      CHECK (odyssey_computer_step (d.cmc, 1000) == 0);
      CHECK (d.lgc->agc.agc_clientdata == d.lgc);
    }
    dual_close (&d);
  }

  /* ---- SEQTEST in both seats: samples never cross over ------------------ */
  {
    CHECK (dual_open (&d, rseq, r1k, 1000) == 0);
    odyssey_scheduler_run (&d.s, 600000);
    CHECK (d.s.capture[ODY_COMPUTER_CMC].samples == 8);     /* the 8 SEQTEST words */
    CHECK (d.cmc->sample_seq == 8);
    CHECK (d.s.capture[ODY_COMPUTER_LGC].samples == d.lgc->sample_seq - d.lgc->samples.dropped);
    CHECK (d.cmc->alarm_restarts == 0 && d.lgc->alarm_restarts == 0);
    dual_close (&d);
  }

  printf ("test_sched: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
