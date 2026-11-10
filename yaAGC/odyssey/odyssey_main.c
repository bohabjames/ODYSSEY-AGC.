/*
 * ODYSSEY-AGC -- Filename: odyssey_main.c
 * Purpose: Offline (non-real-time) host for the ODYSSEY computers.
 *          Single mode (milestones 2-3): run ONE ROM, capture its audio.
 *          Dual mode   (milestone 5):    run a CMC ROM and an LGC ROM under the
 *          deterministic round-robin scheduler, capturing each separately.
 *          No audio device; no host-side synthesis.
 *
 * Usage:
 *   odyssey-agc --rom ROM.bin [--cycles N] [--slice N] [--out samples.csv]
 *   odyssey-agc --cmc-rom CMC.bin --lgc-rom LGC.bin [--cycles N] [--slice N]
 *               [--out-cmc cmc.csv] [--out-lgc lgc.csv]
 *
 * Output: CSV files (seq,cycle,word_octal,value,pcm16) and key=value
 * statistics on stdout, one per line, intended to be parsed by tests.
 * Single mode prints bare keys (unchanged since milestone 3).  Dual mode
 * prints the same keys prefixed "cmc_" / "lgc_".
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include "odyssey_engine.h"
#include "odyssey_capture.h"
#include "odyssey_scheduler.h"

static double
now_seconds (void)
{
  struct timespec ts;
  clock_gettime (CLOCK_MONOTONIC, &ts);
  return (double) ts.tv_sec + (double) ts.tv_nsec * 1e-9;
}

static void
usage (const char *argv0)
{
  fprintf (stderr,
           "usage: %s --rom ROM.bin [--cycles N] [--slice N] [--out samples.csv]\n"
           "       %s --cmc-rom CMC.bin --lgc-rom LGC.bin [--cycles N] [--slice N]\n"
           "                [--out-cmc cmc.csv] [--out-lgc lgc.csv]\n"
           "  --cycles N  total AGC machine cycles to run, per computer (default 1000000)\n"
           "  --slice N   cycles per bounded slice (default %u)\n"
           "  --out FILE  write captured samples as CSV (single mode)\n",
           argv0, argv0, ODY_DEFAULT_SLICE_CYCLES);
}

static void
print_stats (const char *pfx, const char *rom, const odyssey_computer_t *c,
             const ody_capture_t *st, uint32_t slice, double host_s)
{
  double agc_s = (double) c->cycles_executed / (double) AGC_PER_SECOND;
  double span = (st->samples > 1) ? (double) (st->last_cycle - st->first_cycle) : 0.0;
  double cps = (st->samples > 1) ? span / (double) (st->samples - 1) : 0.0;

  printf ("%srom=%s\n", pfx, rom);
  printf ("%sagc_cycles=%" PRIu64 "\n", pfx, c->cycles_executed);
  printf ("%sagc_seconds=%.6f\n", pfx, agc_s);
  printf ("%sslice_cycles=%u\n", pfx, slice);
  printf ("%ssamples=%" PRIu64 "\n", pfx, st->samples);
  printf ("%ssamples_emitted=%" PRIu32 "\n", pfx, c->sample_seq);
  printf ("%squeue_pushed=%" PRIu64 "\n", pfx, c->samples.pushed);
  printf ("%squeue_dropped=%" PRIu64 "\n", pfx, c->samples.dropped);
  printf ("%sseq_gaps=%" PRIu64 "\n", pfx, st->seq_gaps);
  printf ("%sout_of_order=%" PRIu64 "\n", pfx, st->out_of_order);
  printf ("%scycle_regressions=%" PRIu64 "\n", pfx, st->cycle_regressions);
  printf ("%saudio_control_writes=%" PRIu64 "\n", pfx, c->audio_control_writes);
  printf ("%sother_channel_writes=%" PRIu64 "\n", pfx, c->other_channel_writes);
  printf ("%slast_other_channel=%o value=%05o\n", pfx,
          c->last_other_channel < 0 ? 0 : (unsigned) c->last_other_channel,
          (unsigned) c->last_other_value);
  printf ("%salarm_restarts=%" PRIu64 "\n", pfx, c->alarm_restarts);
  printf ("%slast_alarm_bits=%05o\n", pfx, (unsigned) c->last_alarm_bits);
  printf ("%smean_cycles_per_sample=%.3f\n", pfx, cps);
  printf ("%sagc_sample_rate_hz=%.3f\n", pfx,
          cps > 0.0 ? (double) AGC_PER_SECOND / cps : 0.0);
  printf ("%shost_seconds=%.6f\n", pfx, host_s);
  printf ("%shost_speed_vs_realtime=%.1f\n", pfx,
          host_s > 0.0 ? agc_s / host_s : 0.0);
}

static FILE *
open_csv (const char *path)
{
  FILE *f;
  if (path == NULL)
    return NULL;
  f = fopen (path, "w");
  if (f == NULL)
    fprintf (stderr, "cannot open '%s' for writing\n", path);
  else
    fprintf (f, "seq,cycle,word_octal,value,pcm16\n");
  return f;
}

int
main (int argc, char **argv)
{
  const char *rom = NULL, *cmc_rom = NULL, *lgc_rom = NULL;
  const char *out = NULL, *out_cmc = NULL, *out_lgc = NULL;
  uint64_t total = 1000000;
  uint32_t slice = ODY_DEFAULT_SLICE_CYCLES;
  int i, rc, dual;

  for (i = 1; i < argc; i++)
    {
      if (!strcmp (argv[i], "--rom") && i + 1 < argc)
        rom = argv[++i];
      else if (!strcmp (argv[i], "--cmc-rom") && i + 1 < argc)
        cmc_rom = argv[++i];
      else if (!strcmp (argv[i], "--lgc-rom") && i + 1 < argc)
        lgc_rom = argv[++i];
      else if (!strcmp (argv[i], "--cycles") && i + 1 < argc)
        total = strtoull (argv[++i], NULL, 10);
      else if (!strcmp (argv[i], "--slice") && i + 1 < argc)
        slice = (uint32_t) strtoul (argv[++i], NULL, 10);
      else if (!strcmp (argv[i], "--out") && i + 1 < argc)
        out = argv[++i];
      else if (!strcmp (argv[i], "--out-cmc") && i + 1 < argc)
        out_cmc = argv[++i];
      else if (!strcmp (argv[i], "--out-lgc") && i + 1 < argc)
        out_lgc = argv[++i];
      else
        {
          usage (argv[0]);
          return 2;
        }
    }
  dual = (cmc_rom != NULL || lgc_rom != NULL);
  if (slice == 0 || (dual && (cmc_rom == NULL || lgc_rom == NULL || rom != NULL ||
                              out != NULL)) ||
      (!dual && (rom == NULL || out_cmc != NULL || out_lgc != NULL)))
    {
      usage (argv[0]);
      return 2;
    }

  if (!dual)
    {
      /* ---------------------------------------------------- single mode --- */
      odyssey_computer_t *c = (odyssey_computer_t *) malloc (sizeof *c);
      ody_capture_t st;
      FILE *csv = NULL;
      uint64_t done = 0;
      double t0, t1;

      if (c == NULL)
        return 3;
      rc = odyssey_computer_init (c, ODY_COMPUTER_CMC, rom);
      if (rc != 0)
        {
          fprintf (stderr, "odyssey_computer_init failed (%d) for ROM '%s'\n", rc, rom);
          free (c);
          return 4;
        }
      if (out != NULL && (csv = open_csv (out)) == NULL)
        {
          odyssey_computer_destroy (c);
          free (c);
          return 5;
        }
      ody_capture_init (&st, csv);
      t0 = now_seconds ();
      while (done < total)
        {
          uint32_t n = (total - done < slice) ? (uint32_t) (total - done) : slice;
          done += odyssey_computer_step (c, n);
          ody_capture_drain (&st, c);   /* deterministic boundary: drain between slices */
        }
      t1 = now_seconds ();
      print_stats ("", rom, c, &st, slice, t1 - t0);
      if (csv != NULL)
        fclose (csv);
      odyssey_computer_destroy (c);
      free (c);
      return 0;
    }
  else
    {
      /* ------------------------------------------------------ dual mode --- */
      odyssey_computer_t *cmc = (odyssey_computer_t *) malloc (sizeof *cmc);
      odyssey_computer_t *lgc = (odyssey_computer_t *) malloc (sizeof *lgc);
      odyssey_scheduler_t sched;
      FILE *fc = NULL, *fl = NULL;
      double t0, t1;
      int status = 0;

      if (cmc == NULL || lgc == NULL)
        {
          free (cmc);
          free (lgc);
          return 3;
        }
      if ((rc = odyssey_computer_init (cmc, ODY_COMPUTER_CMC, cmc_rom)) != 0)
        {
          fprintf (stderr, "init failed (%d) for CMC ROM '%s'\n", rc, cmc_rom);
          free (cmc);
          free (lgc);
          return 4;
        }
      if ((rc = odyssey_computer_init (lgc, ODY_COMPUTER_LGC, lgc_rom)) != 0)
        {
          fprintf (stderr, "init failed (%d) for LGC ROM '%s'\n", rc, lgc_rom);
          odyssey_computer_destroy (cmc);
          free (cmc);
          free (lgc);
          return 4;
        }
      if ((out_cmc != NULL && (fc = open_csv (out_cmc)) == NULL) ||
          (out_lgc != NULL && (fl = open_csv (out_lgc)) == NULL) ||
          odyssey_scheduler_init (&sched, cmc, lgc, slice) != 0)
        status = 5;
      else
        {
          odyssey_scheduler_set_csv (&sched, ODY_COMPUTER_CMC, fc);
          odyssey_scheduler_set_csv (&sched, ODY_COMPUTER_LGC, fl);
          t0 = now_seconds ();
          odyssey_scheduler_run (&sched, total);
          t1 = now_seconds ();
          print_stats ("cmc_", cmc_rom, cmc, &sched.capture[ODY_COMPUTER_CMC],
                       slice, t1 - t0);
          print_stats ("lgc_", lgc_rom, lgc, &sched.capture[ODY_COMPUTER_LGC],
                       slice, t1 - t0);
          printf ("rounds=%" PRIu64 "\n", sched.rounds);
        }
      if (fc != NULL)
        fclose (fc);
      if (fl != NULL)
        fclose (fl);
      odyssey_computer_destroy (cmc);
      odyssey_computer_destroy (lgc);
      free (cmc);
      free (lgc);
      return status;
    }
}
