/*
 * ODYSSEY-AGC -- Filename: odyssey_rt.h
 * Purpose: milestone 8 phase 1 -- real-time audio core.  Lock-free SPSC PCM
 *          rings (one per computer), the mixer, and the audio render function
 *          that an audio callback / device thread calls.  ODYSSEY EXTENSION.
 *
 * THREAD ROLES (strict)
 *   PRODUCER  = the scheduler thread.  The ONLY thread that calls
 *               ody_rt_push()/ody_rt_sink() and ody_rt_set_active().
 *   CONSUMER  = the audio callback thread (or, offline, the scheduler thread
 *               itself).  The ONLY thread that calls ody_rt_render().
 *   CONTROL   = any other thread (GUI, CLI).  May call every ody_rt_set_* and
 *               ody_rt_get_* function at any time; they are single atomic
 *               operations.
 *
 * TRANSPORT/MIXING ONLY: this module converts the representation of AGC
 * samples (AGC word -> PCM16, zero-order hold to the device rate), scales and
 * sums them.  It never generates a waveform.
 *
 * ody_rt_render() RT CONSTRAINTS (verified by test + code review, see
 * docs/AUDIO-ENGINE.md): no allocation, no locks, no system calls, no
 * stdio, no AGC instructions; a fixed amount of work per frame; only
 * relaxed/acquire/release atomics on the data it shares.
 */
#ifndef ODYSSEY_RT_H
#define ODYSSEY_RT_H

#include <stdatomic.h>
#include <stdint.h>
#include "odyssey_audio.h"

#define ODY_RT_CHANNELS 2            /* index = ODY_COMPUTER_CMC / _LGC       */
#define ODY_RT_GAIN_UNITY 32768u     /* Q15: 1.0                              */
#define ODY_RT_GAIN_MAX (4u * ODY_RT_GAIN_UNITY)   /* +12 dB ceiling          */
#define ODY_RT_CACHELINE 64

typedef struct
{
  /* producer-owned line */
  _Alignas (ODY_RT_CACHELINE) _Atomic uint32_t head;   /* next write (monotonic) */
  /* consumer-owned line */
  _Alignas (ODY_RT_CACHELINE) _Atomic uint32_t tail;   /* next read  (monotonic) */
  _Alignas (ODY_RT_CACHELINE) int16_t *slots;          /* set once at init       */
  uint32_t capacity;                                   /* power of two           */
} ody_rt_ring_t;

typedef struct
{
  ody_rt_ring_t ring;

  /* control -> callback (each a single atomic; relaxed is enough because the
   * callback only needs SOME recent value, never an ordering with other data) */
  _Atomic uint32_t gain_q15;
  _Atomic uint32_t mute;
  _Atomic uint32_t solo;
  _Atomic uint32_t active;      /* producer says: this computer is playing   */
  _Atomic uint32_t prefill;     /* start playing only once this many samples are queued (0 = at once) */

  /* producer-side statistics (written by the producer only) */
  _Atomic uint64_t pushed;
  _Atomic uint64_t dropped;          /* samples refused because ring was full */
  _Atomic uint64_t overrun_events;   /* distinct full episodes                */
  uint32_t full_state;               /* producer-private                       */

  /* consumer-side statistics (written by the callback only) */
  _Atomic uint64_t consumed;
  _Atomic uint64_t underrun_events;  /* distinct starved episodes             */
  _Atomic uint64_t underrun_frames;  /* device frames rendered while starved  */
  _Atomic int32_t peak;              /* max |post-gain| since last read_peak  */

  /* consumer-private state (callback thread only; never read elsewhere) */
  int16_t cur;                       /* held value (zero-order hold)           */
  uint32_t need;                     /* samples wanted but not yet obtained    */
  uint32_t starved;                  /* currently in an underrun episode       */
  uint32_t primed;                   /* prefill satisfied since last activation */
} ody_rt_chan_t;

typedef struct
{
  ody_rt_chan_t chan[ODY_RT_CHANNELS];
  _Atomic uint32_t master_q15;
  uint32_t device_rate;              /* frames/s, fixed at init                */
  uint32_t stream_rate;              /* AGC samples/s mapped onto the stream   */
  uint64_t step_q32;                 /* stream samples per device frame, Q32   */
  uint64_t acc_q32;                  /* consumer-private phase accumulator     */
  _Atomic uint64_t rendered_frames;
  _Atomic uint64_t clipped_frames;
} ody_rt_t;

/* Producer-side feed context for ody_capture's sink hook. */
typedef struct { ody_rt_t *rt; int id; } ody_rt_feed_t;

/* ring_capacity must be a power of two; device_rate/stream_rate > 0.
 * Returns 0 or <0.  Not RT-safe (allocates). */
int  ody_rt_init (ody_rt_t *rt, uint32_t ring_capacity, uint32_t device_rate,
                  uint32_t stream_rate);
void ody_rt_destroy (ody_rt_t *rt);

/* ---- producer (scheduler thread) ---- */
int  ody_rt_push (ody_rt_t *rt, int id, int16_t pcm);   /* 1 stored, 0 dropped */
void ody_rt_sink (void *ctx, const ody_sample_t *s);    /* ody_capture sink    */
void ody_rt_set_active (ody_rt_t *rt, int id, int active);
/* Any thread, before or during playback: the channel stays silent (and is NOT
 * counted as underrunning) until its ring holds at least n samples; this
 * builds the latency cushion that absorbs scheduling jitter.  Applies at the
 * next (re)activation. */
void ody_rt_set_prefill (ody_rt_t *rt, int id, uint32_t n);

/* ---- consumer (audio callback) ----
 * Renders `frames` frames into out (interleaved int16, `channels` = 1 or 2,
 * both output channels carry the same mono mix).  Bounded, lock-free. */
void ody_rt_render (ody_rt_t *rt, int16_t *out, uint32_t frames, uint32_t channels);

/* ---- control (any thread, any time) ---- */
void ody_rt_set_gain_q15 (ody_rt_t *rt, int id, uint32_t q15);   /* clamped */
void ody_rt_set_mute (ody_rt_t *rt, int id, int on);
void ody_rt_set_solo (ody_rt_t *rt, int id, int on);
void ody_rt_set_master_q15 (ody_rt_t *rt, uint32_t q15);          /* clamped */
uint32_t ody_rt_gain_from_float (double g);                        /* -> Q15, clamped */

/* ---- monitoring (any thread) ---- */
typedef struct
{
  uint64_t pushed, dropped, overrun_events, consumed;
  uint64_t underrun_events, underrun_frames;
  uint32_t fill;                      /* samples currently in the ring         */
  int32_t peak;                       /* NOT reset by get_stats                */
} ody_rt_chan_stats_t;
void ody_rt_get_stats (const ody_rt_t *rt, int id, ody_rt_chan_stats_t *out);
int32_t ody_rt_read_peak (ody_rt_t *rt, int id);                   /* read + reset */
uint64_t ody_rt_rendered_frames (const ody_rt_t *rt);
uint64_t ody_rt_clipped_frames (const ody_rt_t *rt);

#endif /* ODYSSEY_RT_H */
