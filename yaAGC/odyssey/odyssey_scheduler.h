/*
 * ODYSSEY-AGC -- Filename: odyssey_scheduler.h
 * Purpose: Deterministic, single-threaded round-robin scheduler for the two
 *          ODYSSEY computers (CMC and LGC).  Milestone 5.
 *
 * OWNERSHIP
 *   The scheduler holds REFERENCES to two odyssey_computer_t objects owned by
 *   the caller.  It never touches their agc_t state itself; it only calls
 *   odyssey_computer_step() and drains their sample queues.  Destroying or
 *   resetting a computer is the caller's job (see odyssey_scheduler_reset_
 *   computer(), which keeps the capture bookkeeping consistent).
 *
 * ONE SCHEDULER ROUND (this order is a contract; tests rely on it)
 *   1. step CMC for `budget` engine calls
 *   2. step LGC for `budget` engine calls
 *   3. drain CMC audio queue  (-> capture stats / CSV)
 *   4. drain LGC audio queue
 *   5. BOUNDARY: odyssey_bus_deliver() (does nothing if no bus is attached)
 *   6. diagnostics bookkeeping (round counter)
 *
 *   Whatever an AGC emits during step 1 or 2 can only become visible to the
 *   OTHER computer at step 5 of the same round, i.e. before that computer's
 *   next step.  Both computers' slices of round N finish before any delivery,
 *   so the delivery boundary is symmetric: neither direction is favoured
 *   (CMC->LGC and LGC->CMC messages both first become readable at the start of
 *   round N+1).  A message is never made visible during an instruction.
 */
#ifndef ODYSSEY_SCHEDULER_H
#define ODYSSEY_SCHEDULER_H

#include <stdint.h>
#include <stdio.h>
#include "odyssey_engine.h"
#include "odyssey_capture.h"
#include "odyssey_bus.h"

typedef struct odyssey_scheduler
{
  odyssey_computer_t *computer[ODY_MAX_COMPUTERS];   /* borrowed, not owned */
  ody_capture_t capture[ODY_MAX_COMPUTERS];
  odyssey_bus_t *bus;          /* borrowed; NULL = no bus                    */
  uint32_t slice;              /* engine calls per computer per round        */
  uint64_t rounds;             /* completed rounds                           */
  uint64_t cycles;             /* engine calls given to EACH computer so far */
  void (*round_hook) (void *ctx, uint64_t cycles);
  void *round_hook_ctx;
} odyssey_scheduler_t;

/* Both computers must be initialised.  slice must be > 0.  Returns 0 or <0. */
int  odyssey_scheduler_init (odyssey_scheduler_t *s, odyssey_computer_t *cmc,
                             odyssey_computer_t *lgc, uint32_t slice);

/* Optional CSV log for one computer's samples (stream not owned). */
void odyssey_scheduler_set_csv (odyssey_scheduler_t *s, int id, FILE *csv);

/* Optional channel-0301 status log for one computer (stream not owned). */
void odyssey_scheduler_set_status_csv (odyssey_scheduler_t *s, int id, FILE *csv);

/* Optional real-time audio feed for one computer (M8): every drained sample
 * is also passed to sink(ctx, sample).  ctx not owned. */
void odyssey_scheduler_set_sink (odyssey_scheduler_t *s, int id,
                                 ody_sample_sink_fn sink, void *ctx);

/* Optional hook run at step 6 of every round (after bus delivery), on the
 * scheduler thread.  Used for offline audio rendering and real-time pacing. */
void odyssey_scheduler_set_round_hook (odyssey_scheduler_t *s,
                                       void (*hook) (void *ctx, uint64_t cycles), void *ctx);

/* Attach a bus (borrowed): both computers' bus pointers are set and delivery
 * runs at step 5 of every round.  Call after both computers are initialised. */
void odyssey_scheduler_attach_bus (odyssey_scheduler_t *s, odyssey_bus_t *bus);

/* Execute one round with `budget` engine calls per computer. */
void odyssey_scheduler_round (odyssey_scheduler_t *s, uint32_t budget);

/* Execute rounds until each computer has been given `total` engine calls
 * (the last round is shortened).  Returns the number of rounds executed. */
uint64_t odyssey_scheduler_run (odyssey_scheduler_t *s, uint64_t total);

/* Reset ONE computer (power-on) and its capture bookkeeping; the other is
 * not touched.  Returns 0 or the odyssey_computer_reset() error. */
int  odyssey_scheduler_reset_computer (odyssey_scheduler_t *s, int id);
/* Audio effect of a reset (M8, docs/AUDIO-ENGINE.md "Reset semantics"): call it
 * only on the scheduler thread BETWEEN rounds.  The computer's CycleCounter
 * restarts at 0, but the stamps delivered to the audio sink stay on the
 * scheduler timeline (reset time + local cycle, never backwards), one rest
 * sample (ODY_SEQ_REST, PCM 0) is delivered at the reset time, samples already
 * queued play out, and the other computer, the mixer and the counters are not
 * touched.  CSV logs keep the computer's own CycleCounter. */

#endif /* ODYSSEY_SCHEDULER_H */
