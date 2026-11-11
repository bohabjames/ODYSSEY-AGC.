/*
 * ODYSSEY-AGC -- Filename: odyssey_bus.h
 * Purpose: In-process virtual spacecraft bus between the CMC and the LGC.
 *          Milestone 6.  ODYSSEY EXTENSION -- not an Apollo interface.
 *
 * The bus never gives one AGC access to the other's memory, registers or
 * synthesis variables.  It only moves COMPLETE four-word messages between
 * host-side queues.  The receiving AGC sees a message through its OWN input
 * channels, which are filled in by the engine-invoked ChannelInput(State)
 * hook for that same State (see odyssey_io.c).
 *
 * ---------------------------------------------------------------------------
 * PROTOCOL
 *
 * Transmit (AGC -> host), same logical channels in both computers:
 *   0310 TYPE   0311 SEQ   0312 DATA0   0313 DATA1   then   0314 COMMIT
 *   Each of the four field writes records the (15-bit masked) value and sets
 *   that field's dirty bit.  Rewriting a field before COMMIT replaces it (last
 *   write wins); the field stays dirty.  A write to 0314 is a COMMIT ATTEMPT,
 *   and ALWAYS clears the whole staging area afterwards, so stale fields can
 *   never complete a later message.  A commit attempt is judged in this order:
 *     1. not all four fields dirty                 -> incomplete        (reject)
 *     2. COMMIT value != staged SEQ                -> invalid_commits   (reject)
 *     3. TYPE == 0 (reserved/invalid)              -> invalid_commits   (reject)
 *     4. sequence check vs the sender's last accepted SEQ (if any):
 *          delta = (SEQ - last) & 077777   (unsigned modular arithmetic)
 *          delta == 0            -> duplicate        (reject)
 *          delta in 1..037777    -> forward: accept; if delta > 1 the skipped
 *                                   numbers are added to seq_gaps
 *          delta in 040000..     -> behind the last one: out_of_order (reject)
 *     5. accepted: appended to this sender's directional queue.  If the queue
 *        is full the message is DROPPED (newest), queue_overflows/dropped are
 *        incremented and the receiver's sticky OVERFLOW status bit is set.
 *        The sender never blocks.  A message dropped for overflow still counts
 *        as the sender's last sequence number (the receiver sees a gap).
 *   The first message after power-on/reset is accepted with any SEQ.
 *   077777 -> 000000 wraps normally (delta == 1).
 *
 * Receive (host -> AGC, ordinary input channels, read with READ):
 *   0320 STATUS  bit 1 (01) VALID, bit 2 (02) OVERFLOW (sticky until reset)
 *   0321 TYPE    0322 SEQ    0323 DATA0    0324 DATA1
 *   All four fields read as 0 when STATUS.VALID is 0.  Channels 0320-0324 are
 *   READ-ONLY for the AGC: the engine's WriteIO() would otherwise store an AGC
 *   write into InputChannel[], so the host republishes them every engine call
 *   and counts any AGC write as rx_write_violations.
 *   Acknowledge (AGC -> host): write the message's SEQ to 0325.  A matching ack
 *   releases the mailbox; a non-matching ack, or an ack with no valid message,
 *   is counted in invalid_acks and changes nothing.
 *
 * Delivery timing (contract with the scheduler):
 *   odyssey_bus_deliver() is called ONCE per scheduler round, after both
 *   computers have finished their slices.  For each computer whose mailbox is
 *   empty it moves the OLDEST queued message from the other computer into the
 *   mailbox.  So a message committed during round N can first be read in
 *   round N+1, in BOTH directions; at most one message per computer per
 *   round is delivered.  A message in the mailbox is stable until acknowledged
 *   (or until that computer is reset).  Because delivery happens only at
 *   boundaries, slice size changes WHEN a message arrives, not WHAT arrives.
 *
 * Reset / resynchronisation of computer X (odyssey_bus_reset_computer):
 *   X's staging area, last-sequence memory, mailbox and incoming queue are
 *   cleared, and so is X's outgoing queue (stale).  Every discarded message is
 *   counted in `dropped`.  The OTHER computer's mailbox is left alone (it may
 *   be mid-read) and keeps being stable until acked.  X's sequence checking
 *   restarts.  Counters are cumulative and are NOT cleared (resets is one).
 * ---------------------------------------------------------------------------
 *
 * Single-threaded; no allocation (all storage is inside odyssey_bus_t).
 */
#ifndef ODYSSEY_BUS_H
#define ODYSSEY_BUS_H

#include <stdint.h>
#include <stdio.h>
#include "odyssey_config.h"

#define ODY_BUS_F_TYPE   01
#define ODY_BUS_F_SEQ    02
#define ODY_BUS_F_DATA0  04
#define ODY_BUS_F_DATA1  010
#define ODY_BUS_F_ALL    017

typedef struct { uint16_t type, seq, data0, data1; } ody_bus_msg_t;

typedef struct
{
  ody_bus_msg_t slots[ODY_BUS_QUEUE_CAPACITY];
  uint32_t head, tail;               /* monotonic; masked on use */
} ody_bus_queue_t;

typedef enum
{
  ODY_BUS_REJECT_NONE = 0,
  ODY_BUS_REJECT_INCOMPLETE,
  ODY_BUS_REJECT_SEQ_MISMATCH,
  ODY_BUS_REJECT_BAD_TYPE,
  ODY_BUS_REJECT_DUPLICATE,
  ODY_BUS_REJECT_OUT_OF_ORDER,
  ODY_BUS_REJECT_QUEUE_FULL          /* accepted but dropped */
} ody_bus_reject_t;

/* Per-computer counters.  tx_messages and the commit-validation counters
 * describe what the computer SENT; rx_messages and acked describe what it
 * RECEIVED.  Cumulative; not cleared by a computer reset. */
typedef struct
{
  uint64_t tx_messages;       /* commits accepted into the queue              */
  uint64_t rx_messages;       /* messages delivered into this mailbox         */
  uint64_t acked;             /* mailbox releases by matching acknowledgement */
  uint64_t invalid_commits;   /* seq mismatch or reserved TYPE                */
  uint64_t incomplete;        /* commit before all four fields                */
  uint64_t duplicates;
  uint64_t out_of_order;
  uint64_t seq_gaps;          /* skipped sequence numbers on accepted commits */
  uint64_t invalid_acks;
  uint64_t queue_overflows;   /* commits that found the queue full            */
  uint64_t dropped;           /* all discarded messages (overflow + reset)    */
  uint64_t rx_write_violations; /* AGC wrote a read-only receive channel      */
  uint64_t resets;            /* reset/resynchronisation events               */
  uint64_t ack_timeouts;      /* mailbox waited > ack_timeout_rounds          */
  uint64_t max_ack_wait_rounds;
} ody_bus_stats_t;

typedef struct
{
  /* sender side */
  uint16_t stage[4];          /* TYPE, SEQ, DATA0, DATA1                      */
  uint8_t dirty;              /* ODY_BUS_F_* mask                             */
  int tx_have_last;
  uint16_t tx_last_seq;
  ody_bus_reject_t last_reject;
  /* receiver side */
  int mb_valid;
  ody_bus_msg_t mb;
  uint32_t mb_age_rounds;     /* rounds the current message has waited        */
  int mb_timeout_flagged;
  int rx_overflow_sticky;
  ody_bus_stats_t st;
} ody_bus_port_t;

typedef struct odyssey_bus
{
  ody_bus_queue_t q[ODY_MAX_COMPUTERS];   /* q[i]: messages SENT BY computer i */
  ody_bus_port_t port[ODY_MAX_COMPUTERS];
  uint32_t ack_timeout_rounds;            /* 0 = timeout reporting disabled   */
  uint64_t deliveries;                    /* odyssey_bus_deliver() calls so far = round index */
  FILE *log;                              /* optional transport log, borrowed; see below */
} odyssey_bus_t;

/* Optional bus log (diagnostics only; the bus never reads it).  One line per
 * event, CSV, "round" = number of deliveries completed so far:
 *   C,round,from,type,seq,data0,data1   accepted commit (queued or dropped-full)
 *   D,round,to,seq                      message moved into a mailbox
 *   K,round,id,seq                      matching acknowledgement
 * Values are octal.  from/to/id: 0 = CMC, 1 = LGC. */

void odyssey_bus_init (odyssey_bus_t *bus);

/* Handle an AGC write to a bus channel (0310-0314, 0320-0325) by computer `id`.
 * Returns 1 if the channel belongs to the bus (handled), 0 otherwise. */
int  odyssey_bus_channel_write (odyssey_bus_t *bus, int id, int channel, int value);

/* Scheduler-boundary delivery (see header comment).  Call once per round. */
void odyssey_bus_deliver (odyssey_bus_t *bus);

/* Fill channels 0320-0324 of the receiving computer's OWN input-channel array
 * from its mailbox.  Called only from ChannelInput(State) for that State. */
void odyssey_bus_publish (const odyssey_bus_t *bus, int id, int16_t *input_channels);

void odyssey_bus_reset_computer (odyssey_bus_t *bus, int id);

/* Messages waiting for computer `id`: queued from the other side + mailbox. */
uint32_t odyssey_bus_pending (const odyssey_bus_t *bus, int id);
uint32_t odyssey_bus_queue_size (const odyssey_bus_t *bus, int from_id);

#endif /* ODYSSEY_BUS_H */
