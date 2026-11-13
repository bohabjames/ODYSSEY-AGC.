/*
 * ODYSSEY-AGC test double: a FAKE <alsa/asoundlib.h>.
 * Declares only the libasound entry points odyssey_backend.c uses, with the
 * prototypes as documented for libasound.  It is NOT the real library; it lets
 * the real alsa backend code compile and run against tests/fake_alsa/fake_alsa.c.
 */
#ifndef ODY_FAKE_ASOUNDLIB_H
#define ODY_FAKE_ASOUNDLIB_H
typedef struct _snd_pcm snd_pcm_t;
typedef long snd_pcm_sframes_t;
typedef unsigned long snd_pcm_uframes_t;
typedef enum { SND_PCM_STREAM_PLAYBACK = 0, SND_PCM_STREAM_CAPTURE = 1 } snd_pcm_stream_t;
typedef enum { SND_PCM_FORMAT_S16_LE = 2 } snd_pcm_format_t;
typedef enum { SND_PCM_ACCESS_RW_INTERLEAVED = 3 } snd_pcm_access_t;

int snd_pcm_open (snd_pcm_t **pcm, const char *name, snd_pcm_stream_t stream, int mode);
int snd_pcm_close (snd_pcm_t *pcm);
int snd_pcm_set_params (snd_pcm_t *pcm, snd_pcm_format_t format, snd_pcm_access_t access,
                        unsigned int channels, unsigned int rate, int soft_resample,
                        unsigned int latency);
snd_pcm_sframes_t snd_pcm_writei (snd_pcm_t *pcm, const void *buffer, snd_pcm_uframes_t size);
int snd_pcm_recover (snd_pcm_t *pcm, int err, int silent);
int snd_pcm_drain (snd_pcm_t *pcm);
const char *snd_strerror (int errnum);
#endif
