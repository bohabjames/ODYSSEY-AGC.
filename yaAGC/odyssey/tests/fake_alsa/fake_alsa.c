/* ODYSSEY-AGC test double: a scriptable fake of the libasound calls the backend uses. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "alsa/asoundlib.h"
#include "fake_alsa.h"

fake_alsa_ctl_t fake_alsa;
struct _snd_pcm { int dummy; };

int
snd_pcm_open (snd_pcm_t **pcm, const char *name, snd_pcm_stream_t stream, int mode)
{
  (void) stream; (void) mode;
  fake_alsa.open_calls++;
  strncpy (fake_alsa.last_name, name, sizeof fake_alsa.last_name - 1);
  if (fake_alsa.open_error)
    return fake_alsa.open_error;
  *pcm = calloc (1, sizeof **pcm);
  return 0;
}

int snd_pcm_close (snd_pcm_t *pcm) { fake_alsa.close_calls++; free (pcm); return 0; }
int snd_pcm_drain (snd_pcm_t *pcm) { (void) pcm; fake_alsa.drain_calls++; return 0; }

int
snd_pcm_set_params (snd_pcm_t *pcm, snd_pcm_format_t f, snd_pcm_access_t a, unsigned int ch,
                    unsigned int rate, int soft_resample, unsigned int latency)
{
  (void) pcm; (void) f; (void) a; (void) latency;
  fake_alsa.set_params_calls++;
  fake_alsa.channels = ch; fake_alsa.rate = rate; fake_alsa.soft_resample = (unsigned) soft_resample;
  return fake_alsa.params_error;
}

snd_pcm_sframes_t
snd_pcm_writei (snd_pcm_t *pcm, const void *buffer, snd_pcm_uframes_t size)
{
  unsigned long n = size;
  struct timespec ts = { 0, 200000 };            /* the "device clock": 0.2 ms per call */
  (void) pcm;
  nanosleep (&ts, NULL);
  if (fake_alsa.dead_after && fake_alsa.got_frames >= fake_alsa.dead_after)
    return -EIO;
  if (fake_alsa.next_xrun < fake_alsa.n_xrun)
    {
      size_t at = fake_alsa.xrun_at[fake_alsa.next_xrun];
      if (fake_alsa.got_frames >= at)
        {
          fake_alsa.next_xrun++;
          return -EPIPE;
        }
      if (n > at - fake_alsa.got_frames)
        n = at - fake_alsa.got_frames;           /* accept up to the fault, then fail next call */
    }
  if (fake_alsa.dead_after && n > fake_alsa.dead_after - fake_alsa.got_frames)
    n = fake_alsa.dead_after - fake_alsa.got_frames;
  if (fake_alsa.max_chunk && n > fake_alsa.max_chunk)
    n = fake_alsa.max_chunk;
  if (fake_alsa.got_frames + n > fake_alsa.cap_frames)
    n = fake_alsa.cap_frames - fake_alsa.got_frames;
  if (n == 0)
    return 0;
  memcpy (fake_alsa.got + fake_alsa.got_frames * fake_alsa.channels, buffer,
          n * fake_alsa.channels * sizeof (int16_t));
  fake_alsa.got_frames += n;
  return (snd_pcm_sframes_t) n;
}

int
snd_pcm_recover (snd_pcm_t *pcm, int err, int silent)
{
  (void) pcm; (void) err; (void) silent;
  fake_alsa.recover_calls++;
  return fake_alsa.recover_ok ? 0 : -EIO;
}

const char *
snd_strerror (int e)
{
  return e == -ENOENT ? "fake: no such device" : e == -EINVAL ? "fake: invalid argument" : "fake: error";
}
