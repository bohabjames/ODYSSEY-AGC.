/*
 * ODYSSEY-AGC -- Filename: odyssey_bus.c
 * See odyssey_bus.h for the protocol and timing contract.
 */
#include <string.h>
#include "odyssey_bus.h"

#define QMASK (ODY_BUS_QUEUE_CAPACITY - 1u)

/* ------------------------------------------------------------------ queue */
static uint32_t
q_size (const ody_bus_queue_t *q)
{
  return q->head - q->tail;
}

static int
q_push (ody_bus_queue_t *q, const ody_bus_msg_t *m)
{
  if (q_size (q) >= ODY_BUS_QUEUE_CAPACITY)
    return 0;                         /* full: caller drops the NEWEST        */
  q->slots[q->head & QMASK] = *m;
  q->head++;
  return 1;
}

static int
q_pop (ody_bus_queue_t *q, ody_bus_msg_t *out)
{
  if (q_size (q) == 0)
    return 0;
  *out = q->slots[q->tail & QMASK];
  q->tail++;
  return 1;
}

void
odyssey_bus_init (odyssey_bus_t *bus)
{
  memset (bus, 0, sizeof *bus);
}

uint32_t
odyssey_bus_queue_size (const odyssey_bus_t *bus, int from_id)
{
  return q_size (&bus->q[from_id]);
}

uint32_t
odyssey_bus_pending (const odyssey_bus_t *bus, int id)
{
  return q_size (&bus->q[1 - id]) + (uint32_t) (bus->port[id].mb_valid != 0);
}

/* --------------------------------------------------------------- transmit */
static void
commit (odyssey_bus_t *bus, int id, uint16_t commit_value)
{
  ody_bus_port_t *p = &bus->port[id];
  ody_bus_msg_t m;
  uint16_t delta;

  m.type = p->stage[0];
  m.seq = p->stage[1];
  m.data0 = p->stage[2];
  m.data1 = p->stage[3];

  if (p->dirty != ODY_BUS_F_ALL)
    {
      p->st.incomplete++;
      p->last_reject = ODY_BUS_REJECT_INCOMPLETE;
      goto done;
    }
  if (commit_value != m.seq)
    {
      p->st.invalid_commits++;
      p->last_reject = ODY_BUS_REJECT_SEQ_MISMATCH;
      goto done;
    }
  if (m.type == 0)
    {
      p->st.invalid_commits++;
      p->last_reject = ODY_BUS_REJECT_BAD_TYPE;
      goto done;
    }
  if (p->tx_have_last)
    {
      /* unsigned modular distance: never compare wrapping numbers as signed */
      delta = (uint16_t) ((m.seq - p->tx_last_seq) & ODY_BUS_SEQ_MASK);
      if (delta == 0)
        {
          p->st.duplicates++;
          p->last_reject = ODY_BUS_REJECT_DUPLICATE;
          goto done;
        }
      if (delta >= 040000)
        {
          p->st.out_of_order++;
          p->last_reject = ODY_BUS_REJECT_OUT_OF_ORDER;
          goto done;
        }
      if (delta > 1)
        p->st.seq_gaps += (uint64_t) (delta - 1);
    }

  if (bus->log != NULL)
    fprintf (bus->log, "C,%llu,%d,%o,%o,%o,%o\n", (unsigned long long) bus->deliveries,
             id, (unsigned) m.type, (unsigned) m.seq, (unsigned) m.data0,
             (unsigned) m.data1);
  /* Accepted.  Even if the queue is full the sequence number counts as sent. */
  p->tx_have_last = 1;
  p->tx_last_seq = m.seq;
  if (q_push (&bus->q[id], &m))
    {
      p->st.tx_messages++;
      p->last_reject = ODY_BUS_REJECT_NONE;
    }
  else
    {
      p->st.queue_overflows++;
      p->st.dropped++;
      p->last_reject = ODY_BUS_REJECT_QUEUE_FULL;
      bus->port[1 - id].rx_overflow_sticky = 1;     /* receiver-visible, sticky */
    }

done:
  /* Every commit attempt, good or bad, clears the staging area. */
  memset (p->stage, 0, sizeof p->stage);
  p->dirty = 0;
}

int
odyssey_bus_channel_write (odyssey_bus_t *bus, int id, int channel, int value)
{
  ody_bus_port_t *p;
  uint16_t v = (uint16_t) (value & ODY_BUS_SEQ_MASK);

  if (bus == NULL || id < 0 || id >= ODY_MAX_COMPUTERS)
    return 0;
  p = &bus->port[id];

  switch (channel)
    {
    case ODY_CH_BUS_TX_TYPE:
      p->stage[0] = v;
      p->dirty |= ODY_BUS_F_TYPE;
      return 1;
    case ODY_CH_BUS_TX_SEQ:
      p->stage[1] = v;
      p->dirty |= ODY_BUS_F_SEQ;
      return 1;
    case ODY_CH_BUS_TX_DATA0:
      p->stage[2] = v;
      p->dirty |= ODY_BUS_F_DATA0;
      return 1;
    case ODY_CH_BUS_TX_DATA1:
      p->stage[3] = v;
      p->dirty |= ODY_BUS_F_DATA1;
      return 1;
    case ODY_CH_BUS_TX_COMMIT:
      commit (bus, id, v);
      return 1;
    case ODY_CH_BUS_RX_ACK:
      if (p->mb_valid && v == p->mb.seq)
        {
          if (bus->log != NULL)
            fprintf (bus->log, "K,%llu,%d,%o\n", (unsigned long long) bus->deliveries,
                     id, (unsigned) p->mb.seq);
          p->mb_valid = 0;
          p->mb_age_rounds = 0;
          p->mb_timeout_flagged = 0;
          p->st.acked++;
        }
      else
        p->st.invalid_acks++;
      return 1;
    case ODY_CH_BUS_RX_STATUS:
    case ODY_CH_BUS_RX_TYPE:
    case ODY_CH_BUS_RX_SEQ:
    case ODY_CH_BUS_RX_DATA0:
    case ODY_CH_BUS_RX_DATA1:
      p->st.rx_write_violations++;      /* read-only for the AGC               */
      return 1;
    default:
      return 0;
    }
}

/* -------------------------------------------------------------- delivery */
void
odyssey_bus_deliver (odyssey_bus_t *bus)
{
  int r;

  /* Each receiver draws only from the OTHER computer's queue and writes only
   * its own port, so the two directions are independent of loop order. */
  for (r = 0; r < ODY_MAX_COMPUTERS; r++)
    {
      ody_bus_port_t *p = &bus->port[r];
      if (!p->mb_valid)
        {
          if (q_pop (&bus->q[1 - r], &p->mb))
            {
              p->mb_valid = 1;
              p->mb_age_rounds = 0;
              p->mb_timeout_flagged = 0;
              p->st.rx_messages++;
              if (bus->log != NULL)
                fprintf (bus->log, "D,%llu,%d,%o\n", (unsigned long long) bus->deliveries,
                         r, (unsigned) p->mb.seq);
            }
        }
      else
        {
          p->mb_age_rounds++;
          if (p->mb_age_rounds > p->st.max_ack_wait_rounds)
            p->st.max_ack_wait_rounds = p->mb_age_rounds;
          if (bus->ack_timeout_rounds != 0 && !p->mb_timeout_flagged &&
              p->mb_age_rounds > bus->ack_timeout_rounds)
            {
              p->mb_timeout_flagged = 1;
              p->st.ack_timeouts++;
            }
        }
    }
  bus->deliveries++;
}

void
odyssey_bus_publish (const odyssey_bus_t *bus, int id, int16_t *ch)
{
  const ody_bus_port_t *p = &bus->port[id];

  ch[ODY_CH_BUS_RX_STATUS] = (int16_t) ((p->mb_valid ? ODY_BUS_STATUS_VALID : 0) |
                                        (p->rx_overflow_sticky ? ODY_BUS_STATUS_OVERFLOW : 0));
  ch[ODY_CH_BUS_RX_TYPE] = (int16_t) (p->mb_valid ? p->mb.type : 0);
  ch[ODY_CH_BUS_RX_SEQ] = (int16_t) (p->mb_valid ? p->mb.seq : 0);
  ch[ODY_CH_BUS_RX_DATA0] = (int16_t) (p->mb_valid ? p->mb.data0 : 0);
  ch[ODY_CH_BUS_RX_DATA1] = (int16_t) (p->mb_valid ? p->mb.data1 : 0);
}

void
odyssey_bus_reset_computer (odyssey_bus_t *bus, int id)
{
  ody_bus_port_t *p;
  uint32_t n;

  if (bus == NULL || id < 0 || id >= ODY_MAX_COMPUTERS)
    return;
  p = &bus->port[id];

  n = q_size (&bus->q[id]) + q_size (&bus->q[1 - id]) + (uint32_t) (p->mb_valid != 0);
  bus->q[id].head = bus->q[id].tail = 0;
  bus->q[1 - id].head = bus->q[1 - id].tail = 0;
  p->st.dropped += n;

  memset (p->stage, 0, sizeof p->stage);
  p->dirty = 0;
  p->tx_have_last = 0;
  p->tx_last_seq = 0;
  p->last_reject = ODY_BUS_REJECT_NONE;
  p->mb_valid = 0;
  memset (&p->mb, 0, sizeof p->mb);
  p->mb_age_rounds = 0;
  p->mb_timeout_flagged = 0;
  p->rx_overflow_sticky = 0;
  p->st.resets++;
}
