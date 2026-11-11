/*
 * ODYSSEY-AGC -- Filename: odyssey_backend.c
 * See odyssey_backend.h.
 */
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "odyssey_backend.h"

#if defined(ODY_WITH_ALSA) && defined(__has_include)
# if __has_include(<alsa/asoundlib.h>)
#  include <alsa/asoundlib.h>
#  define ODY_HAVE_ALSA 1
# endif
#endif

#define AGC_MCT_PER_SECOND 85333ull

/* ------------------------------------------------------------------ WAV */
static void
le32 (unsigned char *p, uint32_t v)
{
  p[0] = v & 255; p[1] = (v >> 8) & 255; p[2] = (v >> 16) & 255; p[3] = (v >> 24) & 255;
}

static void
le16 (unsigned char *p, uint32_t v)
{
  p[0] = v & 255; p[1] = (v >> 8) & 255;
}

static void
wav_header (unsigned char h[44], uint32_t rate, uint32_t ch, uint32_t data_bytes)
{
  memcpy (h, "RIFF", 4); le32 (h + 4, 36 + data_bytes); memcpy (h + 8, "WAVEfmt ", 8);
  le32 (h + 16, 16); le16 (h + 20, 1); le16 (h + 22, ch); le32 (h + 24, rate);
  le32 (h + 28, rate * ch * 2); le16 (h + 32, ch * 2); le16 (h + 34, 16);
  memcpy (h + 36, "data", 4); le32 (h + 40, data_bytes);
}

int
ody_wav_open (ody_wav_t *w, const char *path, uint32_t rate, uint32_t channels)
{
  unsigned char h[44];
  memset (w, 0, sizeof *w);
  w->f = fopen (path, "wb");
  if (w->f == NULL)
    return -1;
  w->rate = rate;
  w->channels = channels;
  wav_header (h, rate, channels, 0);
  return fwrite (h, 1, 44, w->f) == 44 ? 0 : -2;
}

int
ody_wav_write (ody_wav_t *w, const int16_t *s, uint32_t frames)
{
  uint32_t n = frames * w->channels, i;
  unsigned char b[2];
  for (i = 0; i < n; i++)            /* explicit little-endian, host independent */
    {
      le16 (b, (uint16_t) s[i]);
      if (fwrite (b, 1, 2, w->f) != 2)
        return -1;
    }
  w->frames += frames;
  return 0;
}

int
ody_wav_close (ody_wav_t *w)
{
  unsigned char h[44];
  int rc = 0;
  if (w->f == NULL)
    return 0;
  wav_header (h, w->rate, w->channels, (uint32_t) (w->frames * w->channels * 2));
  if (fseek (w->f, 0, SEEK_SET) != 0 || fwrite (h, 1, 44, w->f) != 44)
    rc = -1;
  if (fclose (w->f) != 0)
    rc = -1;
  w->f = NULL;
  return rc;
}

/* -------------------------------------------------------------- offline */
void
ody_offline_init (ody_offline_t *o, ody_rt_t *rt, ody_wav_t *wav, uint32_t channels)
{
  memset (o, 0, sizeof *o);
  o->rt = rt;
  o->wav = wav;
  o->channels = channels;
  o->cycles_per_second = AGC_MCT_PER_SECOND;
}

void
ody_offline_hook (void *ctx, uint64_t cycles)
{
  ody_offline_t *o = ctx;
  uint64_t due = cycles * o->rt->device_rate / o->cycles_per_second;
  while (o->rendered < due)
    {
      uint64_t n = due - o->rendered;
      if (n > 4096)
        n = 4096;
      ody_rt_render (o->rt, o->buf, (uint32_t) n, o->channels);
      if (o->wav != NULL)
        (void) ody_wav_write (o->wav, o->buf, (uint32_t) n);
      o->rendered += n;
    }
}

/* -------------------------------------------------------------- backends */
struct ody_backend
{
  int kind;                      /* 0 paced, 1 alsa */
  ody_rt_t *rt;
  ody_backend_cfg_t cfg;
  pthread_t th;
  int running;
  _Atomic int stop;
  _Atomic uint64_t periods;
  _Atomic uint64_t max_render_ns;
  ody_wav_t wav;
  int16_t *buf;
#ifdef ODY_HAVE_ALSA
  snd_pcm_t *pcm;
#endif
};

static uint64_t
now_ns (void)
{
  struct timespec t;
  clock_gettime (CLOCK_MONOTONIC, &t);
  return (uint64_t) t.tv_sec * 1000000000ull + (uint64_t) t.tv_nsec;
}

static void
timed_render (ody_backend_t *b)
{
  uint64_t t0 = now_ns (), d;
  ody_rt_render (b->rt, b->buf, b->cfg.period_frames, b->cfg.channels);
  d = now_ns () - t0;
  if (d > atomic_load_explicit (&b->max_render_ns, memory_order_relaxed))
    atomic_store_explicit (&b->max_render_ns, d, memory_order_relaxed);
  atomic_fetch_add_explicit (&b->periods, 1, memory_order_relaxed);
}

static void *
paced_thread (void *arg)
{
  ody_backend_t *b = arg;
  uint64_t period_ns = (uint64_t) b->cfg.period_frames * 1000000000ull / b->cfg.rate;
  uint64_t next = now_ns () + period_ns;
  while (!atomic_load_explicit (&b->stop, memory_order_acquire))
    {
      struct timespec ts;
      timed_render (b);
      (void) ody_wav_write (&b->wav, b->buf, b->cfg.period_frames);   /* emulated device sink */
      ts.tv_sec = (time_t) (next / 1000000000ull);
      ts.tv_nsec = (long) (next % 1000000000ull);
      clock_nanosleep (CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
      next += period_ns;
    }
  return NULL;
}

#ifdef ODY_HAVE_ALSA
static void *
alsa_thread (void *arg)
{
  ody_backend_t *b = arg;
  while (!atomic_load_explicit (&b->stop, memory_order_acquire))
    {
      snd_pcm_sframes_t n;
      timed_render (b);
      n = snd_pcm_writei (b->pcm, b->buf, b->cfg.period_frames);   /* blocks on the device clock */
      if (n < 0)
        (void) snd_pcm_recover (b->pcm, (int) n, 1);               /* xrun: recover, keep going */
    }
  return NULL;
}
#endif

ody_backend_t *
ody_backend_create (const char *kind, ody_rt_t *rt, const ody_backend_cfg_t *cfg, char *err,
                    size_t errlen)
{
  ody_backend_t *b;
#define FAIL(msg) do { if (err) snprintf (err, errlen, "%s", msg); free (b); return NULL; } while (0)
  b = calloc (1, sizeof *b);
  if (b == NULL)
    return NULL;
  if (rt == NULL || cfg == NULL || cfg->rate != rt->device_rate || cfg->period_frames == 0 ||
      cfg->period_frames > 65536 || (cfg->channels != 1 && cfg->channels != 2))
    FAIL ("invalid backend configuration (rate must equal the engine device rate)");
  b->rt = rt;
  b->cfg = *cfg;
  atomic_init (&b->stop, 0);
  atomic_init (&b->periods, 0);
  atomic_init (&b->max_render_ns, 0);
  b->buf = calloc ((size_t) cfg->period_frames * cfg->channels, sizeof (int16_t));
  if (b->buf == NULL)
    FAIL ("out of memory");
  if (kind != NULL && strcmp (kind, "paced") == 0)
    {
      b->kind = 0;
      if (cfg->device == NULL || ody_wav_open (&b->wav, cfg->device, cfg->rate, cfg->channels) != 0)
        {
          free (b->buf);
          FAIL ("cannot open WAV output file");
        }
      return b;
    }
  if (kind != NULL && strcmp (kind, "alsa") == 0)
    {
#ifdef ODY_HAVE_ALSA
      unsigned int rate = cfg->rate;
      const char *dev = cfg->device ? cfg->device : "default";
      b->kind = 1;
      if (snd_pcm_open (&b->pcm, dev, SND_PCM_STREAM_PLAYBACK, 0) < 0 ||
          snd_pcm_set_params (b->pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                              cfg->channels, rate, 0,
                              (unsigned int) ((uint64_t) cfg->period_frames * 4 * 1000000 / rate)) < 0)
        {
          if (b->pcm)
            snd_pcm_close (b->pcm);
          free (b->buf);
          FAIL ("cannot open/configure the ALSA device");
        }
      return b;
#else
      free (b->buf);
      FAIL ("this build has no ALSA support (build with ODY_WITH_ALSA=1 and ALSA headers)");
#endif
    }
  free (b->buf);
  FAIL ("unknown backend kind (use paced or alsa)");
#undef FAIL
}

int
ody_backend_start (ody_backend_t *b)
{
  void *(*fn) (void *) = paced_thread;
  if (b == NULL || b->running)
    return -1;
#ifdef ODY_HAVE_ALSA
  if (b->kind == 1)
    fn = alsa_thread;
#endif
  atomic_store_explicit (&b->stop, 0, memory_order_release);
  if (pthread_create (&b->th, NULL, fn, b) != 0)
    return -2;
  b->running = 1;
  return 0;
}

void
ody_backend_stop (ody_backend_t *b)
{
  if (b == NULL)
    return;
  if (b->running)
    {
      atomic_store_explicit (&b->stop, 1, memory_order_release);
      pthread_join (b->th, NULL);
      b->running = 0;
    }
  if (b->kind == 0)
    (void) ody_wav_close (&b->wav);
}

void
ody_backend_destroy (ody_backend_t *b)
{
  if (b == NULL)
    return;
  ody_backend_stop (b);
#ifdef ODY_HAVE_ALSA
  if (b->kind == 1 && b->pcm)
    {
      snd_pcm_drain (b->pcm);
      snd_pcm_close (b->pcm);
    }
#endif
  free (b->buf);
  free (b);
}

uint64_t
ody_backend_periods (const ody_backend_t *b)
{
  return atomic_load_explicit (&((ody_backend_t *) b)->periods, memory_order_relaxed);
}

uint64_t
ody_backend_max_render_ns (const ody_backend_t *b)
{
  return atomic_load_explicit (&((ody_backend_t *) b)->max_render_ns, memory_order_relaxed);
}
