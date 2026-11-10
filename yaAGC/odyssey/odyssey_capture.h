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
} ody_capture_t;

/* csv may be NULL.  The caller owns (opens/closes) the stream and writes any
 * header line. */
void ody_capture_init (ody_capture_t *cap, FILE *csv);

/* Forget ordering history (use after odyssey_computer_reset(), which restarts
 * the computer's sequence numbers at 0).  Keeps the csv stream. */
void ody_capture_reset (ody_capture_t *cap);

/* Pop everything currently queued in c->samples. */
void ody_capture_drain (ody_capture_t *cap, odyssey_computer_t *c);

#endif /* ODYSSEY_CAPTURE_H */
