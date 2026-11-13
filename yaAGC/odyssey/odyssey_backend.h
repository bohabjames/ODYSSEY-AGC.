/*
 * ODYSSEY-AGC -- Filename: odyssey_backend.h
 * Purpose: milestone 8 phase 1 -- audio output backends that drive
 *          ody_rt_render().  ODYSSEY EXTENSION.
 *
 *   offline  : no thread.  After every scheduler round the scheduler thread
 *              itself renders exactly the frames due for the AGC time elapsed
 *              and appends them to a WAV file.  Fully deterministic.
 *   paced    : a device-EMULATION thread that calls ody_rt_render() once per
 *              period in wall-clock time (clock_nanosleep to absolute
 *              deadlines) and writes a WAV file.  It has the timing and
 *              threading of a real audio callback without needing hardware;
 *              file I/O happens in this emulator thread, NOT inside
 *              ody_rt_render().
 *   alsa     : Linux ALSA playback (only built with ODY_WITH_ALSA=1 and ALSA
 *              headers).  The write/recover logic (ody_pcm_write_all) is tested
 *              with scripted fakes, and the real alsa thread against a fake
 *              libasound (tests/fake_alsa); it has NEVER run against the real
 *              libasound or a sound device (none in the development sandbox).
 */
#ifndef ODYSSEY_BACKEND_H
#define ODYSSEY_BACKEND_H

#include <stdint.h>
#include <stdio.h>
#include "odyssey_rt.h"

/* ---- device write loop (used by the alsa backend; device independent) ----
 * Blocking-write contract, like snd_pcm_writei():
 *   writei(dev, buf, frames) -> frames accepted (0 <= n <= frames; may be FEWER
 *                               than asked: a partial write) or -errno.
 *   recover(dev, err)        -> 0 if the stream is usable again (xrun/suspend
 *                               handled), <0 if not.
 * ody_pcm_write_all() delivers EVERY frame of one rendered period, in order,
 * exactly once: after a partial write it continues with the remainder, and after
 * an error it recovers the device and retries the REMAINDER (nothing is skipped
 * and nothing is repeated).  The audio that was already late stays late (the
 * stall becomes extra latency); the engine's own underrun/overrun counters
 * record the consequences.  It gives up (-1, stats->fatal set) only after
 * ODY_PCM_MAX_STUCK consecutive errors/stalls with no progress, so a dead
 * device can never spin the CPU. */
#define ODY_PCM_MAX_STUCK 32
typedef struct
{
  long (*writei) (void *dev, const void *buf, unsigned long frames);
  int (*recover) (void *dev, int err);
  void (*idle) (unsigned usec);            /* NULL = nanosleep; tests inject a no-op */
} ody_pcm_ops_t;

typedef struct
{
  _Atomic uint64_t frames_written;
  _Atomic uint64_t partial_writes;         /* writei accepted fewer frames than asked */
  _Atomic uint64_t xruns;                  /* -EPIPE seen                             */
  _Atomic uint64_t write_errors;           /* every negative writei result            */
  _Atomic uint64_t recoveries;             /* recover() returned 0                    */
  _Atomic uint64_t recovery_failures;
  _Atomic uint64_t stalls;                 /* writei returned 0 frames / -EAGAIN      */
  _Atomic uint64_t frames_abandoned;       /* left unwritten because stop was set     */
  _Atomic int fatal;                       /* last -errno when the loop gave up, else 0 */
} ody_pcm_stats_t;

/* 0 = whole period written, 1 = stopped early (stop flag), -1 = gave up. */
int ody_pcm_write_all (const ody_pcm_ops_t *ops, void *dev, const int16_t *buf, uint32_t frames,
                       uint32_t channels, ody_pcm_stats_t *st, const _Atomic int *stop);

typedef struct
{
  const char *device;        /* alsa: PCM name ("default", "hw:0"); paced: WAV path */
  uint32_t rate;             /* device frames/s (must equal rt->device_rate)         */
  uint32_t period_frames;    /* frames per callback                                  */
  uint32_t channels;         /* 1 or 2                                               */
} ody_backend_cfg_t;

typedef struct ody_backend ody_backend_t;

/* kind: "paced" or "alsa".  Returns NULL on failure (message in err, if non-NULL). */
ody_backend_t *ody_backend_create (const char *kind, ody_rt_t *rt, const ody_backend_cfg_t *cfg,
                                   char *err, size_t errlen);
int  ody_backend_start (ody_backend_t *b);          /* 0 ok */
void ody_backend_stop (ody_backend_t *b);           /* joins the thread, finalizes file */
void ody_backend_destroy (ody_backend_t *b);
uint64_t ody_backend_periods (const ody_backend_t *b);      /* callbacks executed */
/* Worst observed duration of one ody_rt_render() call in ns (measurement). */
uint64_t ody_backend_max_render_ns (const ody_backend_t *b);
/* Device-side health (any thread).  `fatal` != 0: the output thread gave up and
 * NO MORE AUDIO is being rendered (check this; the rings will overrun). */
typedef struct
{
  uint64_t periods, max_render_ns, frames_written, partial_writes, xruns, write_errors,
           recoveries, recovery_failures, stalls, frames_abandoned;
  int fatal;
} ody_backend_stats_t;
void ody_backend_get_stats (const ody_backend_t *b, ody_backend_stats_t *out);

/* ---- WAV (16-bit PCM) writer, used by offline and paced ---- */
typedef struct { FILE *f; uint32_t rate, channels; uint64_t frames; } ody_wav_t;
int  ody_wav_open (ody_wav_t *w, const char *path, uint32_t rate, uint32_t channels);
int  ody_wav_write (ody_wav_t *w, const int16_t *interleaved, uint32_t frames);
int  ody_wav_close (ody_wav_t *w);          /* patches the header sizes */

/* ---- offline renderer (deterministic) ---- */
typedef struct
{
  ody_rt_t *rt;
  ody_wav_t *wav;               /* may be NULL: render and discard */
  uint32_t channels;
  uint64_t rendered;            /* frames rendered so far */
  uint64_t cycles_per_second;   /* AGC MCT per second (85333) */
  int16_t buf[4096 * 2];
} ody_offline_t;
void ody_offline_init (ody_offline_t *o, ody_rt_t *rt, ody_wav_t *wav, uint32_t channels);
/* scheduler round hook: renders frames due at `cycles` AGC cycles */
void ody_offline_hook (void *ctx, uint64_t cycles);

#endif /* ODYSSEY_BACKEND_H */
