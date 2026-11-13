/* Script/inspection interface of the fake libasound (tests only). */
#ifndef ODY_FAKE_ALSA_H
#define ODY_FAKE_ALSA_H
#include <stddef.h>
#include <stdint.h>
typedef struct
{
  /* script (set before ody_backend_create) */
  int open_error;                 /* snd_pcm_open returns this (negative errno)   */
  int params_error;               /* snd_pcm_set_params returns this              */
  unsigned max_chunk;             /* frames accepted per writei (0 = all)         */
  size_t xrun_at[8];              /* inject -EPIPE once accepted frames reach this */
  int n_xrun;
  int recover_ok;                 /* snd_pcm_recover succeeds                     */
  size_t dead_after;              /* every write fails with -EIO after this many frames (0 = never) */
  /* observations */
  int16_t *got;                   /* every frame accepted, interleaved            */
  size_t got_frames, cap_frames;
  unsigned channels, rate;
  int next_xrun, recover_calls, open_calls, close_calls, drain_calls, set_params_calls;
  char last_name[64];
  unsigned soft_resample;
} fake_alsa_ctl_t;
extern fake_alsa_ctl_t fake_alsa;
#endif
