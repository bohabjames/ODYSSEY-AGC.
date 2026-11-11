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
 *              headers).  NOT compiled or tested in the development sandbox
 *              (no ALSA library or sound device there).
 */
#ifndef ODYSSEY_BACKEND_H
#define ODYSSEY_BACKEND_H

#include <stdint.h>
#include <stdio.h>
#include "odyssey_rt.h"

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
