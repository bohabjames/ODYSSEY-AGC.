/*
 * ODYSSEY-AGC -- Filename: tests/test_bus.c
 * Milestone 6: virtual spacecraft bus -- protocol, queues, mailbox, diagnostics
 * and the integration points (ChannelOutput routing, ChannelInput republish,
 * reset).  usage: test_bus ROMDIR   (needs SILENT.bin)
 *
 * Sections 1-13 exercise the bus with no AGC at all, by playing the part of an
 * AGC channel write (odyssey_bus_channel_write).  Section 14 uses real
 * computers and the real engine hooks.  The AGC-instruction level tests (real
 * READ/WRITE) are in run_tests.sh with the BUS-* ROMs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "odyssey_engine.h"
#include "odyssey_bus.h"
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

enum { C = ODY_COMPUTER_CMC, L = ODY_COMPUTER_LGC };

static odyssey_bus_t *
new_bus (void)
{
  odyssey_bus_t *b = (odyssey_bus_t *) calloc (1, sizeof *b);
  odyssey_bus_init (b);
  return b;
}

static void
wr (odyssey_bus_t *b, int id, int ch, int v)
{
  CHECK (odyssey_bus_channel_write (b, id, ch, v) == 1);
}

/* A full, well-formed transmit transaction. */
static void
send_msg (odyssey_bus_t *b, int id, int type, int seq, int d0, int d1, int commit)
{
  wr (b, id, ODY_CH_BUS_TX_TYPE, type);
  wr (b, id, ODY_CH_BUS_TX_SEQ, seq);
  wr (b, id, ODY_CH_BUS_TX_DATA0, d0);
  wr (b, id, ODY_CH_BUS_TX_DATA1, d1);
  wr (b, id, ODY_CH_BUS_TX_COMMIT, commit);
}

static void
sendok (odyssey_bus_t *b, int id, int type, int seq, int d0, int d1)
{
  send_msg (b, id, type, seq, d0, d1, seq);
}

/* What the receiving computer's READ instructions would see. */
typedef struct { int status, type, seq, d0, d1; } view_t;
static view_t
view (const odyssey_bus_t *b, int id)
{
  int16_t ch[512];
  view_t v;
  memset (ch, 0x55, sizeof ch);       /* poison: publish must overwrite them */
  odyssey_bus_publish (b, id, ch);
  v.status = ch[ODY_CH_BUS_RX_STATUS];
  v.type = ch[ODY_CH_BUS_RX_TYPE];
  v.seq = ch[ODY_CH_BUS_RX_SEQ];
  v.d0 = ch[ODY_CH_BUS_RX_DATA0];
  v.d1 = ch[ODY_CH_BUS_RX_DATA1];
  return v;
}

static const ody_bus_stats_t *
st (const odyssey_bus_t *b, int id)
{
  return &b->port[id].st;
}

/* ============================================================ 1. basics */
static void
test_basic_delivery (void)
{
  odyssey_bus_t *b = new_bus ();
  view_t v;
  int i;

  /* nothing visible before any message */
  v = view (b, L);
  CHECK (v.status == 0 && v.type == 0 && v.seq == 0 && v.d0 == 0 && v.d1 == 0);

  sendok (b, C, 3, 042, 012345, 065432);
  CHECK (st (b, C)->tx_messages == 1);
  CHECK (odyssey_bus_queue_size (b, C) == 1);
  CHECK (odyssey_bus_pending (b, L) == 1);

  /* committed, but NOT visible until the scheduler boundary */
  v = view (b, L);
  CHECK (v.status == 0);
  CHECK (view (b, C).status == 0);               /* never echoed to the sender */

  odyssey_bus_deliver (b);
  v = view (b, L);
  CHECK (v.status == ODY_BUS_STATUS_VALID && v.type == 3 && v.seq == 042 &&
         v.d0 == 012345 && v.d1 == 065432);
  CHECK (st (b, L)->rx_messages == 1 && odyssey_bus_queue_size (b, C) == 0);
  CHECK (odyssey_bus_pending (b, L) == 1);        /* the mailbox */

  /* stable while waiting for the acknowledgement */
  sendok (b, C, 4, 043, 1, 2);                    /* a second message queues behind */
  for (i = 0; i < 20; i++)
    {
      odyssey_bus_deliver (b);
      v = view (b, L);
      CHECK (v.status == ODY_BUS_STATUS_VALID && v.seq == 042 && v.type == 3 &&
             v.d0 == 012345 && v.d1 == 065432);
    }
  CHECK (odyssey_bus_queue_size (b, C) == 1);

  /* wrong acknowledgements do not release it */
  wr (b, L, ODY_CH_BUS_RX_ACK, 043);              /* the NEXT message's seq */
  wr (b, L, ODY_CH_BUS_RX_ACK, 0);
  wr (b, L, ODY_CH_BUS_RX_ACK, 077777);
  CHECK (st (b, L)->invalid_acks == 3 && st (b, L)->acked == 0);
  CHECK (view (b, L).seq == 042);
  /* the other computer's ack cannot release L's mailbox */
  wr (b, C, ODY_CH_BUS_RX_ACK, 042);
  CHECK (st (b, C)->invalid_acks == 1 && view (b, L).seq == 042);

  /* correct acknowledgement releases it; the next one appears only at the
   * next boundary, not instantly */
  wr (b, L, ODY_CH_BUS_RX_ACK, 042);
  CHECK (st (b, L)->acked == 1 && view (b, L).status == 0);
  wr (b, L, ODY_CH_BUS_RX_ACK, 042);              /* repeated ack: nothing valid */
  CHECK (st (b, L)->invalid_acks == 4);
  odyssey_bus_deliver (b);
  v = view (b, L);
  CHECK (v.status == ODY_BUS_STATUS_VALID && v.seq == 043 && v.type == 4);
  wr (b, L, ODY_CH_BUS_RX_ACK, 043);
  CHECK (st (b, L)->acked == 2 && odyssey_bus_pending (b, L) == 0);
  free (b);
}

/* ========================================================== 2. incomplete */
static void
test_incomplete (void)
{
  odyssey_bus_t *b = new_bus ();
  int mask, f;
  const int chan[4] = { ODY_CH_BUS_TX_TYPE, ODY_CH_BUS_TX_SEQ, ODY_CH_BUS_TX_DATA0,
                        ODY_CH_BUS_TX_DATA1 };
  uint64_t expect = 0;

  /* every proper subset of the four fields, then commit with a plausible value */
  for (mask = 0; mask < 15; mask++)
    {
      for (f = 0; f < 4; f++)
        if (mask & (1 << f))
          wr (b, C, chan[f], f == 1 ? 7 : 1);
      wr (b, C, ODY_CH_BUS_TX_COMMIT, 7);
      expect++;
      CHECK (st (b, C)->incomplete == expect);
      CHECK (b->port[C].last_reject == ODY_BUS_REJECT_INCOMPLETE);
    }
  CHECK (odyssey_bus_queue_size (b, C) == 0 && st (b, C)->tx_messages == 0);

  /* a commit attempt clears staging: old fields cannot complete a later message */
  wr (b, C, ODY_CH_BUS_TX_TYPE, 1);
  wr (b, C, ODY_CH_BUS_TX_SEQ, 9);
  wr (b, C, ODY_CH_BUS_TX_DATA0, 3);
  wr (b, C, ODY_CH_BUS_TX_COMMIT, 9);             /* incomplete (no DATA1) */
  expect++;
  wr (b, C, ODY_CH_BUS_TX_DATA1, 4);              /* only the missing field now */
  wr (b, C, ODY_CH_BUS_TX_COMMIT, 9);             /* must NOT complete the old one */
  expect++;
  CHECK (st (b, C)->incomplete == expect && odyssey_bus_queue_size (b, C) == 0);

  /* and a clean transaction right afterwards still works */
  sendok (b, C, 1, 9, 3, 4);
  CHECK (odyssey_bus_queue_size (b, C) == 1 && st (b, C)->tx_messages == 1);
  free (b);
}

/* ======================================================== 3. commit checks */
static void
test_commit_validation (void)
{
  odyssey_bus_t *b = new_bus ();
  odyssey_bus_t *c;

  /* commit value mismatch */
  send_msg (b, C, 1, 5, 6, 7, 6);
  CHECK (st (b, C)->invalid_commits == 1 && odyssey_bus_queue_size (b, C) == 0);
  CHECK (b->port[C].last_reject == ODY_BUS_REJECT_SEQ_MISMATCH);
  /* staging was cleared by the failed attempt: the same commit again is incomplete */
  wr (b, C, ODY_CH_BUS_TX_COMMIT, 5);
  CHECK (st (b, C)->incomplete == 1);
  /* a mismatch must not poison sequence tracking */
  sendok (b, C, 1, 5, 6, 7);
  CHECK (st (b, C)->tx_messages == 1 && st (b, C)->duplicates == 0);

  /* values are masked to 15 bits (bit 15 and above ignored) */
  c = new_bus ();
  send_msg (c, C, 0100001, 0100005, 0177777, 0100000, 0100005);
  CHECK (st (c, C)->tx_messages == 1);
  odyssey_bus_deliver (c);
  CHECK (view (c, L).type == 1 && view (c, L).seq == 5 && view (c, L).d0 == 077777 &&
         view (c, L).d1 == 0);
  free (c);

  /* reserved TYPE 0 */
  send_msg (b, C, 0, 6, 1, 1, 6);
  CHECK (st (b, C)->invalid_commits == 2 && b->port[C].last_reject == ODY_BUS_REJECT_BAD_TYPE);

  /* rewriting a field before commit: last write wins, message is still one */
  wr (b, C, ODY_CH_BUS_TX_TYPE, 1);
  wr (b, C, ODY_CH_BUS_TX_TYPE, 2);
  wr (b, C, ODY_CH_BUS_TX_SEQ, 11);
  wr (b, C, ODY_CH_BUS_TX_DATA0, 100);
  wr (b, C, ODY_CH_BUS_TX_DATA0, 200);
  wr (b, C, ODY_CH_BUS_TX_DATA1, 300);
  wr (b, C, ODY_CH_BUS_TX_SEQ, 12);               /* rewrite SEQ ... */
  wr (b, C, ODY_CH_BUS_TX_COMMIT, 12);            /* ... commit matches the rewrite */
  CHECK (st (b, C)->tx_messages == 2);
  odyssey_bus_deliver (b);
  wr (b, L, ODY_CH_BUS_RX_ACK, 5);                /* drain the first message */
  odyssey_bus_deliver (b);
  CHECK (view (b, L).type == 2 && view (b, L).seq == 12 && view (b, L).d0 == 200 &&
         view (b, L).d1 == 300);
  free (b);
}

/* ==================================================== 4. sequence numbers */
static void
test_sequences (void)
{
  odyssey_bus_t *b = new_bus ();
  uint64_t n = 0;

  /* first message after power-on: any SEQ */
  sendok (b, C, 1, 012345, 0, 0);
  CHECK (st (b, C)->tx_messages == 1 && st (b, C)->seq_gaps == 0);

  /* normal increment, then wraparound 077776 -> 077777 -> 0 -> 1 */
  free (b);
  b = new_bus ();
  sendok (b, C, 1, 077776, 0, 0);
  sendok (b, C, 1, 077777, 0, 0);
  sendok (b, C, 1, 0, 0, 0);
  sendok (b, C, 1, 1, 0, 0);
  CHECK (st (b, C)->tx_messages == 4 && st (b, C)->seq_gaps == 0 &&
         st (b, C)->duplicates == 0 && st (b, C)->out_of_order == 0);

  /* duplicate */
  sendok (b, C, 1, 1, 9, 9);
  CHECK (st (b, C)->duplicates == 1 && st (b, C)->tx_messages == 4);
  CHECK (b->port[C].last_reject == ODY_BUS_REJECT_DUPLICATE);

  /* older than the last one, across the wrap (last=1, send 077777) */
  sendok (b, C, 1, 077777, 0, 0);
  CHECK (st (b, C)->out_of_order == 1 && st (b, C)->tx_messages == 4);
  CHECK (b->port[C].last_reject == ODY_BUS_REJECT_OUT_OF_ORDER);
  sendok (b, C, 1, 0, 0, 0);                      /* delta = 077777: behind by one */
  CHECK (st (b, C)->out_of_order == 2);

  /* rejected messages must not move the sequence reference: 2 is still next */
  sendok (b, C, 1, 2, 0, 0);
  CHECK (st (b, C)->tx_messages == 5 && st (b, C)->seq_gaps == 0);

  /* forward gap is accepted and counted: 2 -> 5 skips 3 and 4 */
  sendok (b, C, 1, 5, 0, 0);
  CHECK (st (b, C)->tx_messages == 6 && st (b, C)->seq_gaps == 2);
  /* gap across the wrap: 5 -> 077777 is a big forward jump (delta 077772) so it
   * is "behind" under the half-range rule */
  sendok (b, C, 1, 077777, 0, 0);
  CHECK (st (b, C)->out_of_order == 3);

  /* half-range boundary: delta 037777 is the largest accepted forward jump,
   * delta 040000 is the smallest rejected one.  last = 5. */
  sendok (b, C, 1, (5 + 040000) & 077777, 0, 0);
  CHECK (st (b, C)->out_of_order == 4 && st (b, C)->tx_messages == 6);
  sendok (b, C, 1, (5 + 037777) & 077777, 0, 0);
  CHECK (st (b, C)->tx_messages == 7 && st (b, C)->seq_gaps == 2 + 037776);

  /* sequence tracking is per sender: the LGC has its own */
  sendok (b, L, 1, 1, 0, 0);
  CHECK (st (b, L)->tx_messages == 1 && st (b, L)->duplicates == 0);

  /* wraparound is unsigned: a full lap of increments never trips the checks */
  free (b);
  b = new_bus ();
  for (n = 0; n < 70000; n++)
    {
      sendok (b, C, 1, (int) (n & 077777), 0, 0);
      if ((n & 0xff) == 0xff)
        {
          odyssey_bus_deliver (b);
          while (view (b, L).status)
            {
              wr (b, L, ODY_CH_BUS_RX_ACK, view (b, L).seq);
              odyssey_bus_deliver (b);
            }
        }
    }
  CHECK (st (b, C)->duplicates == 0 && st (b, C)->out_of_order == 0 &&
         st (b, C)->seq_gaps == 0 && st (b, C)->queue_overflows == 0);
  CHECK (st (b, C)->tx_messages == 70000);
  free (b);
}

/* ========================================================= 5. queue full */
static void
test_queue_full (void)
{
  odyssey_bus_t *b = new_bus ();
  int i, expect_seq = 0, got = 0;
  view_t v;

  CHECK (ODY_BUS_QUEUE_CAPACITY >= 256);
  for (i = 0; i < (int) ODY_BUS_QUEUE_CAPACITY; i++)
    sendok (b, C, 1, i, i, i ^ 077777);
  CHECK (st (b, C)->tx_messages == ODY_BUS_QUEUE_CAPACITY && st (b, C)->queue_overflows == 0);
  CHECK (view (b, L).status == 0);                       /* nothing sticky yet */

  /* the next ones are dropped, newest first, and the sender is never blocked
   * (every write above returned immediately: this loop completing proves it) */
  sendok (b, C, 1, i, 0, 0);
  sendok (b, C, 1, i + 1, 0, 0);
  CHECK (st (b, C)->queue_overflows == 2 && st (b, C)->dropped == 2 &&
         st (b, C)->tx_messages == ODY_BUS_QUEUE_CAPACITY);
  CHECK (b->port[C].last_reject == ODY_BUS_REJECT_QUEUE_FULL);
  CHECK (odyssey_bus_queue_size (b, C) == ODY_BUS_QUEUE_CAPACITY);
  v = view (b, L);
  CHECK ((v.status & ODY_BUS_STATUS_OVERFLOW) != 0);     /* sticky bit visible to the receiver */
  CHECK ((view (b, C).status & ODY_BUS_STATUS_OVERFLOW) == 0);   /* only the receiver's side */

  /* the retained messages are the OLDEST ones, in order, with intact payloads */
  for (;;)
    {
      odyssey_bus_deliver (b);
      v = view (b, L);
      if (!(v.status & ODY_BUS_STATUS_VALID))
        break;
      CHECK (v.seq == expect_seq && v.d0 == expect_seq && v.d1 == (expect_seq ^ 077777));
      wr (b, L, ODY_CH_BUS_RX_ACK, v.seq);
      expect_seq++;
      got++;
    }
  CHECK (got == (int) ODY_BUS_QUEUE_CAPACITY);
  /* still sticky after draining; the dropped messages left a visible gap */
  CHECK ((view (b, L).status & ODY_BUS_STATUS_OVERFLOW) != 0);
  /* Sender can send again once there is room.  The two dropped messages (SEQ i
   * and i+1) still count as "sent" (documented), so the reference is i+1 and the
   * next accepted SEQ i+5 skips exactly i+2, i+3, i+4 = 3.  The receiver sees the
   * gap as SEQ (i-1) followed by (i+5). */
  sendok (b, C, 1, i + 5, 0, 0);
  CHECK (st (b, C)->tx_messages == ODY_BUS_QUEUE_CAPACITY + 1 && st (b, C)->seq_gaps == 3);
  /* the other direction is unaffected by this one's overflow */
  sendok (b, L, 1, 1, 0, 0);
  CHECK (st (b, L)->queue_overflows == 0 && st (b, L)->tx_messages == 1);
  free (b);
}

/* ================================================= 6. missing acknowledgement */
static void
test_missing_ack (void)
{
  odyssey_bus_t *b = new_bus ();
  int i;

  b->ack_timeout_rounds = 3;
  sendok (b, C, 1, 1, 10, 20);
  sendok (b, C, 1, 2, 30, 40);
  odyssey_bus_deliver (b);                       /* seq 1 into the mailbox */
  for (i = 0; i < 10; i++)
    odyssey_bus_deliver (b);                     /* no acknowledgement ever comes */
  CHECK (view (b, L).seq == 1 && view (b, L).d0 == 10);     /* still there, unchanged */
  CHECK (st (b, L)->acked == 0 && st (b, L)->rx_messages == 1);
  CHECK (odyssey_bus_queue_size (b, C) == 1);               /* seq 2 is held back */
  CHECK (st (b, L)->ack_timeouts == 1);                     /* flagged once, not every round */
  CHECK (st (b, L)->max_ack_wait_rounds == 10);
  CHECK (odyssey_bus_pending (b, L) == 2);
  wr (b, L, ODY_CH_BUS_RX_ACK, 1);
  odyssey_bus_deliver (b);
  CHECK (view (b, L).seq == 2);
  CHECK (st (b, L)->ack_timeouts == 1);
  for (i = 0; i < 5; i++)
    odyssey_bus_deliver (b);
  CHECK (st (b, L)->ack_timeouts == 2);                     /* the next message timed out too */
  free (b);

  /* timeout reporting is off by default */
  b = new_bus ();
  sendok (b, C, 1, 1, 0, 0);
  for (i = 0; i < 50; i++)
    odyssey_bus_deliver (b);
  CHECK (st (b, L)->ack_timeouts == 0 && st (b, L)->max_ack_wait_rounds == 49);
  free (b);
}

/* ========================================== 7. both directions at once */
static void
test_bidirectional (void)
{
  odyssey_bus_t *b = new_bus ();
  view_t vc, vl;

  sendok (b, C, 1, 100, 111, 112);
  sendok (b, L, 2, 200, 211, 212);
  sendok (b, C, 1, 101, 113, 114);
  odyssey_bus_deliver (b);                        /* one boundary serves both directions */
  vl = view (b, L);
  vc = view (b, C);
  CHECK (vl.status == 1 && vl.type == 1 && vl.seq == 100 && vl.d0 == 111 && vl.d1 == 112);
  CHECK (vc.status == 1 && vc.type == 2 && vc.seq == 200 && vc.d0 == 211 && vc.d1 == 212);
  /* acking in one direction does not affect the other */
  wr (b, L, ODY_CH_BUS_RX_ACK, 100);
  CHECK (view (b, L).status == 0 && view (b, C).status == 1 && view (b, C).seq == 200);
  odyssey_bus_deliver (b);
  CHECK (view (b, L).seq == 101 && view (b, C).seq == 200);
  CHECK (st (b, C)->tx_messages == 2 && st (b, L)->tx_messages == 1);
  CHECK (st (b, C)->rx_messages == 1 && st (b, L)->rx_messages == 2);
  free (b);
}

/* ============================================================ 8. reset */
static void
test_reset (void)
{
  odyssey_bus_t *b = new_bus ();

  /* CMC -> LGC: three messages (one in the LGC mailbox, two queued).
   * LGC -> CMC: two messages (one in the CMC mailbox, one queued). */
  sendok (b, C, 1, 10, 1, 1);
  sendok (b, C, 1, 11, 2, 2);
  sendok (b, C, 1, 12, 3, 3);
  sendok (b, L, 2, 20, 4, 4);
  sendok (b, L, 2, 21, 5, 5);
  odyssey_bus_deliver (b);
  CHECK (odyssey_bus_pending (b, L) == 3 && odyssey_bus_pending (b, C) == 2);
  wr (b, L, ODY_CH_BUS_TX_TYPE, 1);               /* LGC also has a half-staged message */

  odyssey_bus_reset_computer (b, L);
  /* LGC: mailbox, incoming queue, outgoing queue and staging all gone */
  CHECK (view (b, L).status == 0);
  CHECK (odyssey_bus_queue_size (b, C) == 0 && odyssey_bus_queue_size (b, L) == 0);
  CHECK (b->port[L].dirty == 0 && b->port[L].tx_have_last == 0);
  CHECK (st (b, L)->resets == 1 && st (b, C)->resets == 0);
  /* 3 for the LGC (mailbox + 2 queued) + 1 queued LGC->CMC message = 4 discarded */
  CHECK (st (b, L)->dropped == 4);
  /* the CMC's mailbox is untouched and still stable until it acks */
  CHECK (view (b, C).status == 1 && view (b, C).seq == 20);
  odyssey_bus_deliver (b);
  CHECK (view (b, C).seq == 20);
  wr (b, C, ODY_CH_BUS_RX_ACK, 20);
  CHECK (st (b, C)->acked == 1);

  /* resynchronised: the LGC may restart its sequence anywhere, no duplicate/ooo */
  sendok (b, L, 2, 5, 0, 0);
  CHECK (st (b, L)->duplicates == 0 && st (b, L)->out_of_order == 0 && st (b, L)->tx_messages == 3);
  /* counters are cumulative across the reset */
  CHECK (st (b, L)->tx_messages == 3 && st (b, C)->tx_messages == 3);
  /* sticky overflow is cleared by a reset */
  b->port[L].rx_overflow_sticky = 1;
  odyssey_bus_reset_computer (b, L);
  CHECK ((view (b, L).status & ODY_BUS_STATUS_OVERFLOW) == 0 && st (b, L)->resets == 2);
  /* invalid arguments are ignored */
  odyssey_bus_reset_computer (b, 7);
  odyssey_bus_reset_computer (NULL, 0);
  free (b);
}

/* ===================================== 9. channels the bus owns / does not */
static void
test_channel_ownership (void)
{
  odyssey_bus_t *b = new_bus ();
  view_t v;
  int ch;

  sendok (b, C, 1, 1, 2, 3);
  odyssey_bus_deliver (b);
  /* receive channels are read-only for the AGC: writes are counted and ignored */
  for (ch = ODY_CH_BUS_RX_STATUS; ch <= ODY_CH_BUS_RX_DATA1; ch++)
    wr (b, L, ch, 0123);
  CHECK (st (b, L)->rx_write_violations == 5);
  v = view (b, L);
  CHECK (v.status == 1 && v.type == 1 && v.seq == 1 && v.d0 == 2 && v.d1 == 3);
  /* channels the bus does not own are not claimed */
  CHECK (odyssey_bus_channel_write (b, C, ODY_CH_AUDIO_SAMPLE, 1) == 0);
  CHECK (odyssey_bus_channel_write (b, C, ODY_CH_AUDIO_CONTROL, 1) == 0);
  CHECK (odyssey_bus_channel_write (b, C, 0315, 1) == 0);
  CHECK (odyssey_bus_channel_write (b, C, 0326, 1) == 0);
  CHECK (odyssey_bus_channel_write (b, C, 011, 1) == 0);
  CHECK (odyssey_bus_channel_write (b, 5, ODY_CH_BUS_TX_TYPE, 1) == 0);
  CHECK (odyssey_bus_channel_write (b, -1, ODY_CH_BUS_TX_TYPE, 1) == 0);
  CHECK (odyssey_bus_channel_write (NULL, C, ODY_CH_BUS_TX_TYPE, 1) == 0);
  free (b);
}

/* ============================================ 10. real computers and hooks */
static void
test_integration (const char *romdir)
{
  char rom[600];
  odyssey_computer_t *a = (odyssey_computer_t *) calloc (1, sizeof *a);
  odyssey_computer_t *l = (odyssey_computer_t *) calloc (1, sizeof *l);
  odyssey_bus_t *bus = new_bus ();
  odyssey_scheduler_t s;
  uint64_t other_before;

  snprintf (rom, sizeof rom, "%s/SILENT.bin", romdir);
  CHECK (odyssey_computer_init (a, C, rom) == 0 && odyssey_computer_init (l, L, rom) == 0);
  CHECK (a->bus == NULL && l->bus == NULL);

  /* without a bus the bus channels behave like any other unhandled channel */
  other_before = a->other_channel_writes;
  ChannelOutput (&a->agc, ODY_CH_BUS_TX_TYPE, 1);
  CHECK (a->other_channel_writes == other_before + 1);

  CHECK (odyssey_scheduler_init (&s, a, l, 1000) == 0);
  odyssey_scheduler_attach_bus (&s, bus);
  CHECK (a->bus == bus && l->bus == bus);

  /* ChannelOutput routes by the State that executed the write */
  other_before = a->other_channel_writes;
  ChannelOutput (&a->agc, ODY_CH_BUS_TX_TYPE, 7);
  ChannelOutput (&a->agc, ODY_CH_BUS_TX_SEQ, 70);
  ChannelOutput (&a->agc, ODY_CH_BUS_TX_DATA0, 071);
  ChannelOutput (&a->agc, ODY_CH_BUS_TX_DATA1, 072);
  ChannelOutput (&a->agc, ODY_CH_BUS_TX_COMMIT, 70);
  CHECK (a->other_channel_writes == other_before);     /* consumed by the bus */
  CHECK (st (bus, C)->tx_messages == 1 && st (bus, L)->tx_messages == 0);

  /* the message reaches the LGC only at the scheduler boundary */
  CHECK (odyssey_computer_step (l, 3) == 3);
  CHECK (l->agc.InputChannel[ODY_CH_BUS_RX_STATUS] == 0);
  odyssey_scheduler_round (&s, 1);                      /* ends with delivery */
  CHECK (l->agc.InputChannel[ODY_CH_BUS_RX_STATUS] == 0);   /* not yet in L's own array ... */
  CHECK (odyssey_computer_step (l, 1) == 1);                /* ... until the engine calls ChannelInput */
  CHECK (l->agc.InputChannel[ODY_CH_BUS_RX_STATUS] == ODY_BUS_STATUS_VALID);
  CHECK (l->agc.InputChannel[ODY_CH_BUS_RX_TYPE] == 7 && l->agc.InputChannel[ODY_CH_BUS_RX_SEQ] == 70 &&
         l->agc.InputChannel[ODY_CH_BUS_RX_DATA0] == 071 && l->agc.InputChannel[ODY_CH_BUS_RX_DATA1] == 072);
  /* the sender's own receive channels are not touched */
  CHECK (odyssey_computer_step (a, 1) == 1);
  CHECK (a->agc.InputChannel[ODY_CH_BUS_RX_STATUS] == 0 && a->agc.InputChannel[ODY_CH_BUS_RX_SEQ] == 0);

  /* the engine's WriteIO() stores AGC writes into InputChannel[]; the next
   * engine call must restore the mailbox so the AGC cannot corrupt it */
  l->agc.InputChannel[ODY_CH_BUS_RX_TYPE] = 0777;
  l->agc.InputChannel[ODY_CH_BUS_RX_STATUS] = 0;
  CHECK (odyssey_computer_step (l, 1) == 1);
  CHECK (l->agc.InputChannel[ODY_CH_BUS_RX_TYPE] == 7 &&
         l->agc.InputChannel[ODY_CH_BUS_RX_STATUS] == ODY_BUS_STATUS_VALID);

  /* acknowledge through the real hook: routed to the LGC's own port */
  ChannelOutput (&l->agc, ODY_CH_BUS_RX_ACK, 70);
  CHECK (st (bus, L)->acked == 1 && st (bus, C)->acked == 0);
  CHECK (odyssey_computer_step (l, 1) == 1);
  CHECK (l->agc.InputChannel[ODY_CH_BUS_RX_STATUS] == 0 && l->agc.InputChannel[ODY_CH_BUS_RX_SEQ] == 0);

  /* odyssey_computer_reset() resynchronises THAT computer's bus state only */
  ChannelOutput (&a->agc, ODY_CH_BUS_TX_TYPE, 1);
  ChannelOutput (&a->agc, ODY_CH_BUS_TX_SEQ, 71);
  ChannelOutput (&a->agc, ODY_CH_BUS_TX_DATA0, 0);
  ChannelOutput (&a->agc, ODY_CH_BUS_TX_DATA1, 0);
  ChannelOutput (&a->agc, ODY_CH_BUS_TX_COMMIT, 71);
  ChannelOutput (&l->agc, ODY_CH_BUS_TX_TYPE, 2);
  ChannelOutput (&l->agc, ODY_CH_BUS_TX_SEQ, 81);
  ChannelOutput (&l->agc, ODY_CH_BUS_TX_DATA0, 0);
  ChannelOutput (&l->agc, ODY_CH_BUS_TX_DATA1, 0);
  ChannelOutput (&l->agc, ODY_CH_BUS_TX_COMMIT, 81);
  odyssey_bus_deliver (bus);
  CHECK (bus->port[C].mb_valid && bus->port[L].mb_valid);
  CHECK (odyssey_computer_reset (l) == 0);
  CHECK (!bus->port[L].mb_valid && bus->port[C].mb_valid);
  CHECK (st (bus, L)->resets == 1 && st (bus, C)->resets == 0);
  CHECK (l->bus == bus);                                /* attachment survives a reset */

  /* destroying a computer detaches it from the bus */
  odyssey_computer_destroy (l);
  CHECK (l->bus == NULL && a->bus == bus);
  odyssey_computer_destroy (a);
  free (a);
  free (l);
  free (bus);
}

int
main (int argc, char **argv)
{
  if (argc < 2)
    {
      fprintf (stderr, "usage: %s ROMDIR\n", argv[0]);
      return 2;
    }
  test_basic_delivery ();
  test_incomplete ();
  test_commit_validation ();
  test_sequences ();
  test_queue_full ();
  test_missing_ack ();
  test_bidirectional ();
  test_reset ();
  test_channel_ownership ();
  test_integration (argv[1]);
  printf ("test_bus: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
