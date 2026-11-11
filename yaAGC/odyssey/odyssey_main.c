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
 *               [--out-cmc cmc.csv] [--out-lgc lgc.csv] [--bus] [--ack-timeout N]
 *   --status F / --status-cmc F / --status-lgc F  log channel-0301 words as CSV
 *             (seq,cycle,word_octal); the host only records them (M7).
 *   --bus-log F  with --bus: log every committed/delivered/acked message (CSV).
 *   --bus attaches the virtual spacecraft bus (milestone 6) and prints
 *   per-computer bus counters ("cmc_bus_*" / "lgc_bus_*", "bus_*").
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
#include "odyssey_bus.h"

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
           "                [--out-cmc cmc.csv] [--out-lgc lgc.csv] [--bus] [--ack-timeout N]\n"
           "  --cycles N  total AGC machine cycles to run, per computer (default 1000000)\n"
           "  --slice N   cycles per bounded slice (default %u)\n"
           "  --out FILE  write captured samples as CSV (single mode)\n"
           "  --bus       dual mode: attach the virtual bus and print its counters\n"
           "  --ack-timeout N  report mailbox acks pending longer than N rounds\n",
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

static void
print_bus_stats (const char *pfx, const odyssey_bus_t *bus, int id)
{
  const ody_bus_stats_t *b = &bus->port[id].st;
  printf ("%sbus_tx_messages=%" PRIu64 "\n", pfx, b->tx_messages);
  printf ("%sbus_rx_messages=%" PRIu64 "\n", pfx, b->rx_messages);
  printf ("%sbus_acked=%" PRIu64 "\n", pfx, b->acked);
  printf ("%sbus_invalid_commits=%" PRIu64 "\n", pfx, b->invalid_commits);
  printf ("%sbus_incomplete=%" PRIu64 "\n", pfx, b->incomplete);
  printf ("%sbus_duplicates=%" PRIu64 "\n", pfx, b->duplicates);
  printf ("%sbus_out_of_order=%" PRIu64 "\n", pfx, b->out_of_order);
  printf ("%sbus_seq_gaps=%" PRIu64 "\n", pfx, b->seq_gaps);
  printf ("%sbus_invalid_acks=%" PRIu64 "\n", pfx, b->invalid_acks);
  printf ("%sbus_queue_overflows=%" PRIu64 "\n", pfx, b->queue_overflows);
  printf ("%sbus_dropped=%" PRIu64 "\n", pfx, b->dropped);
  printf ("%sbus_rx_write_violations=%" PRIu64 "\n", pfx, b->rx_write_violations);
  printf ("%sbus_resets=%" PRIu64 "\n", pfx, b->resets);
  printf ("%sbus_ack_timeouts=%" PRIu64 "\n", pfx, b->ack_timeouts);
  printf ("%sbus_max_ack_wait_rounds=%" PRIu64 "\n", pfx, b->max_ack_wait_rounds);
  printf ("%sbus_pending=%" PRIu32 "\n", pfx, odyssey_bus_pending (bus, id));
  printf ("%sbus_overflow_sticky=%d\n", pfx, bus->port[id].rx_overflow_sticky);
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
  const char *st_single = NULL, *st_cmc = NULL, *st_lgc = NULL, *bus_log_path = NULL;
  uint64_t total = 1000000;
  uint32_t slice = ODY_DEFAULT_SLICE_CYCLES;
  int i, rc, dual, use_bus = 0;
  uint32_t ack_timeout = 0;

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
      else if (!strcmp (argv[i], "--status") && i + 1 < argc)
        st_single = argv[++i];
      else if (!strcmp (argv[i], "--status-cmc") && i + 1 < argc)
        st_cmc = argv[++i];
      else if (!strcmp (argv[i], "--status-lgc") && i + 1 < argc)
        st_lgc = argv[++i];
      else if (!strcmp (argv[i], "--bus-log") && i + 1 < argc)
        bus_log_path = argv[++i];
      else if (!strcmp (argv[i], "--bus"))
        use_bus = 1;
      else if (!strcmp (argv[i], "--ack-timeout") && i + 1 < argc)
        ack_timeout = (uint32_t) strtoul (argv[++i], NULL, 10);
      else
        {
          usage (argv[0]);
          return 2;
        }
    }
  dual = (cmc_rom != NULL || lgc_rom != NULL);
  if (slice == 0 || (dual && (cmc_rom == NULL || lgc_rom == NULL || rom != NULL ||
                              out != NULL)) ||
      (!dual && (rom == NULL || out_cmc != NULL || out_lgc != NULL || use_bus ||
                 st_cmc != NULL || st_lgc != NULL || bus_log_path != NULL)) ||
      (dual && st_single != NULL) || (bus_log_path != NULL && !use_bus))
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
      if (st_single != NULL)
        {
          st.status_csv = fopen (st_single, "w");
          if (st.status_csv == NULL)
            fprintf (stderr, "cannot open '%s' for writing\n", st_single);
          else
            fprintf (st.status_csv, "seq,cycle,word_octal\n");
        }
      t0 = now_seconds ();
      while (done < total)
        {
          uint32_t n = (total - done < slice) ? (uint32_t) (total - done) : slice;
          done += odyssey_computer_step (c, n);
          ody_capture_drain (&st, c);   /* deterministic boundary: drain between slices */
        }
      t1 = now_seconds ();
      print_stats ("", rom, c, &st, slice, t1 - t0);
      printf ("status_words=%" PRIu64 "\n", st.status_words);
      if (csv != NULL)
        fclose (csv);
      if (st.status_csv != NULL)
        fclose (st.status_csv);
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
      odyssey_bus_t *bus = use_bus ? (odyssey_bus_t *) calloc (1, sizeof *bus) : NULL;
      FILE *fc = NULL, *fl = NULL, *fsc = NULL, *fsl = NULL, *flog = NULL;
      double t0, t1;
      int status = 0;

      if (cmc == NULL || lgc == NULL || (use_bus && bus == NULL))
        {
          free (cmc);
          free (lgc);
          free (bus);
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
          if (st_cmc != NULL && (fsc = fopen (st_cmc, "w")) != NULL)
            fprintf (fsc, "seq,cycle,word_octal\n");
          if (st_lgc != NULL && (fsl = fopen (st_lgc, "w")) != NULL)
            fprintf (fsl, "seq,cycle,word_octal\n");
          odyssey_scheduler_set_status_csv (&sched, ODY_COMPUTER_CMC, fsc);
          odyssey_scheduler_set_status_csv (&sched, ODY_COMPUTER_LGC, fsl);
          if (bus != NULL)
            {
              odyssey_bus_init (bus);
              if (bus_log_path != NULL && (flog = fopen (bus_log_path, "w")) != NULL)
                {
                  fprintf (flog, "event,round,who,a,b,c,d\n");
                  bus->log = flog;
                }
              bus->ack_timeout_rounds = ack_timeout;
              odyssey_scheduler_attach_bus (&sched, bus);
            }
          t0 = now_seconds ();
          odyssey_scheduler_run (&sched, total);
          t1 = now_seconds ();
          print_stats ("cmc_", cmc_rom, cmc, &sched.capture[ODY_COMPUTER_CMC],
                       slice, t1 - t0);
          print_stats ("lgc_", lgc_rom, lgc, &sched.capture[ODY_COMPUTER_LGC],
                       slice, t1 - t0);
          printf ("rounds=%" PRIu64 "\n", sched.rounds);
          printf ("cmc_status_words=%" PRIu64 "\n", sched.capture[ODY_COMPUTER_CMC].status_words);
          printf ("lgc_status_words=%" PRIu64 "\n", sched.capture[ODY_COMPUTER_LGC].status_words);
          if (bus != NULL)
            {
              print_bus_stats ("cmc_", bus, ODY_COMPUTER_CMC);
              print_bus_stats ("lgc_", bus, ODY_COMPUTER_LGC);
              printf ("bus_queue_capacity=%u\n", (unsigned) ODY_BUS_QUEUE_CAPACITY);
            }
        }
      if (fc != NULL)
        fclose (fc);
      if (fl != NULL)
        fclose (fl);
      if (fsc != NULL)
        fclose (fsc);
      if (fsl != NULL)
        fclose (fsl);
      if (flog != NULL)
        fclose (flog);
      odyssey_computer_destroy (cmc);
      odyssey_computer_destroy (lgc);
      free (cmc);
      free (lgc);
      free (bus);
      return status;
    }
}
