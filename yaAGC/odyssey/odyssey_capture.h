/*
 * ODYSSEY-AGC -- Filename: odyssey_capture.h
 * Purpose: Drain one computer's audio-sample queue, validate ordering, keep
 *          statistics, and optionally log each sample to a CSV stream.
 *          (Moved out of odyssey_main.c in milestone 5 so the single-computer
 *          host, the dual scheduler and the tests all share one implementation.)
 *
 * TRANSPORT/DIAGNOSTICS ONLY: nothing here computes any waveform.
 */
#ifndef ODYSSEY_CAPTURE_H
#define ODYSSEY_CAPTURE_H

#include <stdio.h>
#include <stdint.h>
#include "odyssey_engine.h"

/* Optional per-sample hook (M8): called for every drained audio sample, on the
 * draining (scheduler) thread.  Used to feed the real-time rings. */
typedef void (*ody_sample_sink_fn) (void *ctx, const ody_sample_t *s);

/* ODYSSEY EXTENSION (host convention, not Apollo): the sample handed to the
 * sink on a computer reset.  word 0 (+0), seq == ODY_SEQ_REST, cycle == the
 * reset time on the scheduler timeline.  It is the AGC's own power-on output
 * level (channel registers are zero after reset), not a synthesized waveform. */
#define ODY_SEQ_REST 0xFFFFFFFFu

typedef struct
{
  uint64_t samples;           /* samples drained                              */
  uint64_t seq_gaps;          /* total missing sequence numbers observed      */
  uint64_t out_of_order;      /* seq went backwards or repeated               */
  uint64_t cycle_regressions; /* cycle timestamp decreased                    */
  uint64_t first_cycle, last_cycle;
  uint32_t expected_seq;
  int have_prev;
  uint64_t prev_cycle;
  FILE *csv;                  /* optional, not owned                          */
  FILE *status_csv;           /* optional channel-0301 log (seq,cycle,word_octal) */
  uint64_t status_words;      /* status words drained                         */
  uint64_t status_seq_gaps;
  uint64_t cycle_base;        /* timeline offset added to the stamp handed to the
                               * SINK only (CSV/validation keep the computer's own
                               * CycleCounter).  0 until a reset rebases it. */
  ody_sample_sink_fn sink;    /* optional, not owned */
  void *sink_ctx;
} ody_capture_t;

/* csv may be NULL.  The caller owns (opens/closes) the stream and writes any
 * header line. */
void ody_capture_init (ody_capture_t *cap, FILE *csv);

/* Forget ordering history (use after odyssey_computer_reset(), which restarts
 * the computer's sequence numbers at 0).  Keeps the csv stream. */
void ody_capture_reset (ody_capture_t *cap);   /* keeps csv and status_csv */

/* After a computer reset, put the sink's stamps back on the scheduler timeline:
 * later samples are delivered with stamp = timeline_cycle + the computer's new
 * local CycleCounter, and one rest sample (see ODY_SEQ_REST) is delivered at
 * timeline_cycle.  Call right after ody_capture_reset(), at a round boundary. */
void ody_capture_rebase (ody_capture_t *cap, uint64_t timeline_cycle, uint8_t computer);

/* Pop everything currently queued in c->samples. */
void ody_capture_drain (ody_capture_t *cap, odyssey_computer_t *c);

#endif /* ODYSSEY_CAPTURE_H */
