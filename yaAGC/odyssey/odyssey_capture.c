/*
 * ODYSSEY-AGC -- Filename: odyssey_capture.c
 * See odyssey_capture.h.
 */
#include <string.h>
#include <inttypes.h>
#include "odyssey_capture.h"

void
ody_capture_init (ody_capture_t *cap, FILE *csv)
{
  memset (cap, 0, sizeof *cap);
  cap->csv = csv;
}

void
ody_capture_reset (ody_capture_t *cap)
{
  FILE *csv = cap->csv, *status_csv = cap->status_csv;
  memset (cap, 0, sizeof *cap);
  cap->csv = csv;
  cap->status_csv = status_csv;
}

void
ody_capture_drain (ody_capture_t *cap, odyssey_computer_t *c)
{
  ody_sample_t s;
  uint32_t status_expected = (uint32_t) cap->status_words + (uint32_t) cap->status_seq_gaps;

  while (ody_queue_pop (&c->status, &s))
    {
      if (s.seq > status_expected)
        cap->status_seq_gaps += s.seq - status_expected;
      status_expected = s.seq + 1;
      cap->status_words++;
      if (cap->status_csv != NULL)
        fprintf (cap->status_csv, "%" PRIu32 ",%" PRIu64 ",%05o\n", s.seq, s.cycle,
                 (unsigned) s.word);
    }
  while (ody_queue_pop (&c->samples, &s))
    {
      if (s.seq > cap->expected_seq)
        cap->seq_gaps += s.seq - cap->expected_seq;
      else if (s.seq < cap->expected_seq)
        cap->out_of_order++;
      cap->expected_seq = s.seq + 1;

      if (cap->have_prev && s.cycle < cap->prev_cycle)
        cap->cycle_regressions++;
      if (!cap->have_prev)
        cap->first_cycle = s.cycle;
      cap->prev_cycle = cap->last_cycle = s.cycle;
      cap->have_prev = 1;
      cap->samples++;

      if (cap->csv != NULL)
        fprintf (cap->csv, "%" PRIu32 ",%" PRIu64 ",%05o,%d,%d\n", s.seq,
                 s.cycle, (unsigned) s.word, ody_agc_to_value (s.word),
                 (int) ody_agc_to_pcm16 (s.word));
    }
}
