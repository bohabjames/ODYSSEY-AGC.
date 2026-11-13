/*
 * ODYSSEY-AGC -- Filename: odyssey_rt.c
 * See odyssey_rt.h for thread roles and constraints.
 *
 * MEMORY ORDERING (SPSC ring)
 *   head is written only by the producer, tail only by the consumer.
 *   push:  h = head.load(relaxed)          own index
 *          t = tail.load(acquire)          pairs with pop's release store of
 *                                          tail: the consumer has finished
 *                                          READING the slot before we reuse it
 *          slots[h & m] = v                plain store
 *          head.store(h + 1, release)      publishes the slot to the consumer
 *   pop:   t = tail.load(relaxed)          own index
 *          h = head.load(acquire)          pairs with push's release store of
 *                                          head: the slot WRITE is visible
 *          v = slots[t & m]                plain load
 *          tail.store(t + 1, release)      hands the slot back
 *   Indices are free-running uint32; fullness is (h - t) >= capacity, which is
 *   correct across wrap because capacity is a power of two <= 2^31.
 */
#include <stdlib.h>
#include <string.h>
#include "odyssey_rt.h"

#define AGC_MCT_PER_SECOND 85333ull
#define MAX_POPS_PER_FRAME 256u   /* bounds the work of a catch-up frame in timed mode */
#define RLX memory_order_relaxed
#define ACQ memory_order_acquire
#define REL memory_order_release

static int
pow2 (uint32_t v)
{
  return v >= 2 && v <= (1u << 30) && (v & (v - 1)) == 0;
}

int
ody_rt_init (ody_rt_t *rt, uint32_t cap, uint32_t device_rate, uint32_t stream_rate)
{
  int i;
  if (rt == NULL || !pow2 (cap) || device_rate == 0)
    return -1;
  memset (rt, 0, sizeof *rt);
  for (i = 0; i < ODY_RT_CHANNELS; i++)
    {
      ody_rt_chan_t *c = &rt->chan[i];
      c->ring.slots = calloc (cap, sizeof (ody_rt_slot_t));
      if (c->ring.slots == NULL)
        {
          ody_rt_destroy (rt);
          return -2;
        }
      c->ring.capacity = cap;
      atomic_init (&c->ring.head, 0);
      atomic_init (&c->ring.tail, 0);
      atomic_init (&c->gain_q15, ODY_RT_GAIN_UNITY);
      atomic_init (&c->mute, 0);
      atomic_init (&c->solo, 0);
      atomic_init (&c->active, 0);
      atomic_init (&c->prefill, 0);
      atomic_init (&c->watermark, 0);
      atomic_init (&c->pushed, 0);
      atomic_init (&c->dropped, 0);
      atomic_init (&c->overrun_events, 0);
      atomic_init (&c->consumed, 0);
      atomic_init (&c->underrun_events, 0);
      atomic_init (&c->underrun_frames, 0);
      atomic_init (&c->peak, 0);
    }
  atomic_init (&rt->master_q15, ODY_RT_GAIN_UNITY);
  /* Runtime counterpart of the _Static_asserts in odyssey_rt.h: the objects
   * the callback really uses, as laid out on this target. */
  if (!atomic_is_lock_free (&rt->chan[0].ring.head) || !atomic_is_lock_free (&rt->chan[0].ring.tail) ||
      !atomic_is_lock_free (&rt->chan[0].watermark) || !atomic_is_lock_free (&rt->chan[0].underrun_frames) ||
      !atomic_is_lock_free (&rt->chan[0].gain_q15) || !atomic_is_lock_free (&rt->chan[0].peak) ||
      !atomic_is_lock_free (&rt->master_q15) || !atomic_is_lock_free (&rt->rendered_frames))
    {
      ody_rt_destroy (rt);
      return -3;
    }
  atomic_init (&rt->rendered_frames, 0);
  atomic_init (&rt->clipped_frames, 0);
  rt->device_rate = device_rate;
  rt->stream_rate = stream_rate;
  rt->timed = (stream_rate == 0);
  rt->step_q32 = ((uint64_t) stream_rate << 32) / device_rate;
  return 0;
}

void
ody_rt_destroy (ody_rt_t *rt)
{
  int i;
  if (rt == NULL)
    return;
  for (i = 0; i < ODY_RT_CHANNELS; i++)
    {
      free (rt->chan[i].ring.slots);
      rt->chan[i].ring.slots = NULL;
    }
}

/* ------------------------------------------------------------- producer */
int
ody_rt_push (ody_rt_t *rt, int id, int16_t pcm)
{
  return ody_rt_push_timed (rt, id, pcm, 0);
}

int
ody_rt_push_timed (ody_rt_t *rt, int id, int16_t pcm, uint64_t cycle)
{
  ody_rt_chan_t *c;
  ody_rt_ring_t *r;
  uint32_t h, t;

  if (id < 0 || id >= ODY_RT_CHANNELS)
    return 0;
  c = &rt->chan[id];
  r = &c->ring;
  h = atomic_load_explicit (&r->head, RLX);
  t = atomic_load_explicit (&r->tail, ACQ);
  if (h - t >= r->capacity)
    {
      atomic_fetch_add_explicit (&c->dropped, 1, RLX);
      if (!c->full_state)
        {
          c->full_state = 1;
          atomic_fetch_add_explicit (&c->overrun_events, 1, RLX);
        }
      return 0;
    }
  c->full_state = 0;
  r->slots[h & (r->capacity - 1)].pcm = pcm;
  r->slots[h & (r->capacity - 1)].cycle = cycle;
  atomic_store_explicit (&r->head, h + 1, REL);
  atomic_fetch_add_explicit (&c->pushed, 1, RLX);
  return 1;
}

void
ody_rt_sink (void *ctx, const ody_sample_t *s)
{
  ody_rt_feed_t *f = ctx;
  (void) ody_rt_push_timed (f->rt, f->id, ody_agc_to_pcm16 (s->word), s->cycle);
}

void
ody_rt_set_active (ody_rt_t *rt, int id, int active)
{
  if (id >= 0 && id < ODY_RT_CHANNELS)
    atomic_store_explicit (&rt->chan[id].active, active ? 1u : 0u, RLX);
}

void
ody_rt_set_watermark (ody_rt_t *rt, int id, uint64_t cycle)
{
  if (id >= 0 && id < ODY_RT_CHANNELS)
    atomic_store_explicit (&rt->chan[id].watermark, cycle, REL);
}

void
ody_rt_publish_watermark (ody_rt_t *rt, uint64_t cycle)
{
  int i;
  for (i = 0; i < ODY_RT_CHANNELS; i++)
    ody_rt_set_watermark (rt, i, cycle);
}

void
ody_rt_set_latency_cycles (ody_rt_t *rt, int64_t cycles)
{
  rt->latency_cycles = cycles < 0 ? 0 : cycles;
}

int64_t
ody_rt_latency_ms_to_cycles (uint32_t ms)
{
  return (int64_t) ((uint64_t) ms * AGC_MCT_PER_SECOND / 1000ull);
}

uint64_t
ody_rt_frames_ready (const ody_rt_t *rt, uint64_t watermark)
{
  if (rt->timed)
    {
      /* frames f with floor(f*MCT/rate) - L <= W  <=>  f < (W + L + 1) * rate / MCT */
      uint64_t x = (watermark + (uint64_t) rt->latency_cycles + 1) * rt->device_rate;
      return (x + AGC_MCT_PER_SECOND - 1) / AGC_MCT_PER_SECOND;
    }
  return watermark * rt->device_rate / AGC_MCT_PER_SECOND;
}

void
ody_rt_set_prefill (ody_rt_t *rt, int id, uint32_t n)
{
  if (id >= 0 && id < ODY_RT_CHANNELS)
    atomic_store_explicit (&rt->chan[id].prefill, n, RLX);
}

/* ------------------------------------------------------------- consumer */
static int
ring_pop (ody_rt_ring_t *r, int16_t *out)
{
  uint32_t t = atomic_load_explicit (&r->tail, RLX);
  uint32_t h = atomic_load_explicit (&r->head, ACQ);
  if (t == h)
    return 0;
  *out = r->slots[t & (r->capacity - 1)].pcm;
  atomic_store_explicit (&r->tail, t + 1, REL);
  return 1;
}

static int32_t
scale (int32_t v, uint32_t q15)
{
  /* Q15 multiply, round to nearest, arithmetic shift (gcc/clang). */
  return (int32_t) (((int64_t) v * (int64_t) q15 + 16384) >> 15);
}

void
ody_rt_render (ody_rt_t *rt, int16_t *out, uint32_t frames, uint32_t channels)
{
  uint32_t gain[ODY_RT_CHANNELS], active[ODY_RT_CHANNELS], audible[ODY_RT_CHANNELS];
  uint64_t wm[ODY_RT_CHANNELS];
  uint32_t master, any_solo = 0, f, i, k;
  uint64_t clipped = 0;
  uint64_t starved_frames[ODY_RT_CHANNELS] = { 0 };
  uint64_t got[ODY_RT_CHANNELS] = { 0 };
  int32_t peak[ODY_RT_CHANNELS] = { 0 };

  /* One snapshot of every control per call: coherent for the whole buffer. */
  for (i = 0; i < ODY_RT_CHANNELS; i++)
    {
      ody_rt_chan_t *c = &rt->chan[i];
      gain[i] = atomic_load_explicit (&c->gain_q15, RLX);
      active[i] = atomic_load_explicit (&c->active, RLX);
      wm[i] = atomic_load_explicit (&c->watermark, ACQ);
      any_solo |= atomic_load_explicit (&c->solo, RLX) != 0;
    }
  for (i = 0; i < ODY_RT_CHANNELS; i++)
    {
      ody_rt_chan_t *c = &rt->chan[i];
      uint32_t m = atomic_load_explicit (&c->mute, RLX) != 0;
      uint32_t s = atomic_load_explicit (&c->solo, RLX) != 0;
      /* mute always wins; if any channel is soloed only soloed ones sound */
      audible[i] = !m && (!any_solo || s);
    }
  master = atomic_load_explicit (&rt->master_q15, RLX);

  for (f = 0; f < frames; f++)
    {
      int32_t mix = 0, post;
      int64_t m64;

      int64_t T = 0;
      if (rt->timed)
        T = (int64_t) ((rt->frame_idx + f) * AGC_MCT_PER_SECOND / rt->device_rate) - rt->latency_cycles;
      else
        {
          rt->acc_q32 += rt->step_q32;
          while (rt->acc_q32 >= (1ull << 32))
            {
              rt->acc_q32 -= (1ull << 32);
              for (i = 0; i < ODY_RT_CHANNELS; i++)
                if (rt->chan[i].need < 1)       /* never accumulate a backlog */
                  rt->chan[i].need = 1;
            }
        }
      for (i = 0; i < ODY_RT_CHANNELS; i++)
        {
          ody_rt_chan_t *c = &rt->chan[i];
          int32_t v;
          if (!active[i])
            {
              c->need = 0;
              c->starved = 0;
              c->primed = 0;
              continue;
            }
          if (rt->timed)
            {
              c->primed = 1;
              if (T >= 0)
                {
                  uint64_t limit = (uint64_t) T;
                  uint32_t n;
                  if (limit > wm[i])
                    {                       /* the producer has not reached this AGC time */
                      limit = wm[i];        /* ...but everything it HAS delivered is playable */
                      if (!c->starved)
                        {
                          c->starved = 1;
                          atomic_fetch_add_explicit (&c->underrun_events, 1, RLX);
                        }
                    }
                  else
                    c->starved = 0;
                  for (n = 0; n < MAX_POPS_PER_FRAME; n++)
                    {
                      uint32_t t = atomic_load_explicit (&c->ring.tail, RLX);
                      uint32_t hd = atomic_load_explicit (&c->ring.head, ACQ);
                      const ody_rt_slot_t *sl;
                      if (t == hd)
                        break;
                      sl = &c->ring.slots[t & (c->ring.capacity - 1)];
                      if (sl->cycle > limit)
                        break;
                      c->cur = sl->pcm;
                      atomic_store_explicit (&c->ring.tail, t + 1, REL);
                      got[i]++;
                    }
                }
            }
          else if (!c->primed)
            {
              uint32_t pf = atomic_load_explicit (&c->prefill, RLX);
              if (pf > 0)
                {
                  uint32_t fill = atomic_load_explicit (&c->ring.head, ACQ) -
                                  atomic_load_explicit (&c->ring.tail, RLX);
                  if (fill < pf)
                    continue;               /* still filling: silent, not an underrun */
                  c->need = 1;              /* start with the first queued sample */
                }
              c->primed = 1;
            }
          if (!rt->timed && c->need > 0)
            {
              int16_t s;
              if (ring_pop (&c->ring, &s))
                {
                  c->cur = s;
                  c->need = 0;
                  got[i]++;
                  c->starved = 0;
                }
              else
                {
                  if (!c->starved)
                    {
                      c->starved = 1;
                      atomic_fetch_add_explicit (&c->underrun_events, 1, RLX);
                    }
                }
            }
          if (c->starved)
            starved_frames[i]++;
          v = scale (c->cur, gain[i]);
          if (v > 32767) v = 32767;
          if (v < -32767) v = -32767;
          if ((v < 0 ? -v : v) > peak[i])
            peak[i] = v < 0 ? -v : v;
          if (audible[i])
            mix += v;
        }
      m64 = ((int64_t) mix * (int64_t) master + 16384) >> 15;
      if (m64 > 32767) { m64 = 32767; clipped++; }
      else if (m64 < -32767) { m64 = -32767; clipped++; }
      post = (int32_t) m64;
      for (k = 0; k < channels; k++)
        out[f * channels + k] = (int16_t) post;
    }

  for (i = 0; i < ODY_RT_CHANNELS; i++)
    {
      ody_rt_chan_t *c = &rt->chan[i];
      if (got[i])
        atomic_fetch_add_explicit (&c->consumed, got[i], RLX);
      if (starved_frames[i])
        atomic_fetch_add_explicit (&c->underrun_frames, starved_frames[i], RLX);
      /* load+store, not CAS (no retry loop in the callback).  A concurrent
       * ody_rt_read_peak() can make this overwrite the reset with a value that
       * is still a true post-gain peak of this period: harmless, at worst the
       * reader sees one period's peak twice. */
      if (peak[i] > atomic_load_explicit (&c->peak, RLX))
        atomic_store_explicit (&c->peak, peak[i], RLX);
    }
  rt->frame_idx += frames;
  atomic_fetch_add_explicit (&rt->rendered_frames, frames, RLX);
  if (clipped)
    atomic_fetch_add_explicit (&rt->clipped_frames, clipped, RLX);
}

/* -------------------------------------------------------------- control */
static uint32_t
clamp_gain (uint32_t q15)
{
  return q15 > ODY_RT_GAIN_MAX ? ODY_RT_GAIN_MAX : q15;
}

void
ody_rt_set_gain_q15 (ody_rt_t *rt, int id, uint32_t q15)
{
  if (id >= 0 && id < ODY_RT_CHANNELS)
    atomic_store_explicit (&rt->chan[id].gain_q15, clamp_gain (q15), RLX);
}

void
ody_rt_set_mute (ody_rt_t *rt, int id, int on)
{
  if (id >= 0 && id < ODY_RT_CHANNELS)
    atomic_store_explicit (&rt->chan[id].mute, on ? 1u : 0u, RLX);
}

void
ody_rt_set_solo (ody_rt_t *rt, int id, int on)
{
  if (id >= 0 && id < ODY_RT_CHANNELS)
    atomic_store_explicit (&rt->chan[id].solo, on ? 1u : 0u, RLX);
}

void
ody_rt_set_master_q15 (ody_rt_t *rt, uint32_t q15)
{
  atomic_store_explicit (&rt->master_q15, clamp_gain (q15), RLX);
}

uint32_t
ody_rt_gain_from_float (double g)
{
  if (!(g > 0.0))
    return 0;
  if (g >= 4.0)
    return ODY_RT_GAIN_MAX;
  return (uint32_t) (g * ODY_RT_GAIN_UNITY + 0.5);
}

/* ----------------------------------------------------------- monitoring */
void
ody_rt_get_stats (const ody_rt_t *rt, int id, ody_rt_chan_stats_t *o)
{
  const ody_rt_chan_t *c;
  uint32_t h, t;
  memset (o, 0, sizeof *o);
  if (id < 0 || id >= ODY_RT_CHANNELS)
    return;
  c = &rt->chan[id];
  o->pushed = atomic_load_explicit (&c->pushed, RLX);
  o->dropped = atomic_load_explicit (&c->dropped, RLX);
  o->overrun_events = atomic_load_explicit (&c->overrun_events, RLX);
  o->consumed = atomic_load_explicit (&c->consumed, RLX);
  o->underrun_events = atomic_load_explicit (&c->underrun_events, RLX);
  o->underrun_frames = atomic_load_explicit (&c->underrun_frames, RLX);
  o->peak = atomic_load_explicit (&c->peak, RLX);
  /* tail first, then head: the producer can only have moved head FORWARD since,
   * so h - t never wraps negative; it can overshoot the true value by the pops
   * that happened in between, hence the clamp. */
  t = atomic_load_explicit (&c->ring.tail, ACQ);
  h = atomic_load_explicit (&c->ring.head, ACQ);
  o->fill = (h - t) > c->ring.capacity ? c->ring.capacity : (h - t);
}

uint32_t
ody_rt_get_gain_q15 (const ody_rt_t *rt, int id)
{
  return (id < 0 || id >= ODY_RT_CHANNELS) ? 0 : atomic_load_explicit (&rt->chan[id].gain_q15, RLX);
}

int
ody_rt_get_mute (const ody_rt_t *rt, int id)
{
  return (id < 0 || id >= ODY_RT_CHANNELS) ? 0 : atomic_load_explicit (&rt->chan[id].mute, RLX) != 0;
}

int
ody_rt_get_solo (const ody_rt_t *rt, int id)
{
  return (id < 0 || id >= ODY_RT_CHANNELS) ? 0 : atomic_load_explicit (&rt->chan[id].solo, RLX) != 0;
}

uint32_t
ody_rt_get_master_q15 (const ody_rt_t *rt)
{
  return atomic_load_explicit (&rt->master_q15, RLX);
}

int32_t
ody_rt_read_peak (ody_rt_t *rt, int id)
{
  if (id < 0 || id >= ODY_RT_CHANNELS)
    return 0;
  return atomic_exchange_explicit (&rt->chan[id].peak, 0, RLX);
}

uint64_t
ody_rt_rendered_frames (const ody_rt_t *rt)
{
  return atomic_load_explicit (&rt->rendered_frames, RLX);
}

uint64_t
ody_rt_clipped_frames (const ody_rt_t *rt)
{
  return atomic_load_explicit (&rt->clipped_frames, RLX);
}
