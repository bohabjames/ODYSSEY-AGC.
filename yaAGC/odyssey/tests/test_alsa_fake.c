/*
 * ODYSSEY-AGC -- Filename: tests/test_alsa_fake.c
 * Runs the REAL alsa backend code (odyssey_backend.c built with ODY_WITH_ALSA)
 * against a scriptable FAKE libasound (tests/fake_alsa).  This exercises the
 * thread, the open/configure error handling, partial writes, xrun recovery and
 * a dead device.  It does NOT test the real libasound or a sound device.
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "odyssey_backend.h"
#include "fake_alsa/fake_alsa.h"

static long checks, failures;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; \
    printf ("FAIL %s:%d: ", __FILE__, __LINE__); printf (__VA_ARGS__); printf ("\n"); } } while (0)

#define RATE 48000u
#define PERIOD 480u
#define NS 60000u                          /* samples pushed: AGC cycles 0..NS-1 */

static void
msleep (unsigned ms)
{
  struct timespec ts = { ms / 1000, (long) (ms % 1000) * 1000000L };
  nanosleep (&ts, NULL);
}

static void
fill (ody_rt_t *rt)
{
  uint32_t c;
  if (ody_rt_init (rt, 1u << 16, RATE, 0) != 0) { puts ("init"); exit (2); }
  for (c = 0; c < NS; c++)
    ody_rt_push_timed (rt, 0, (int16_t) ((c * 13u) % 20000u) - 10000, c);
  ody_rt_set_mute (rt, 1, 1);
  ody_rt_set_active (rt, 0, 1);
  ody_rt_set_active (rt, 1, 0);
  ody_rt_publish_watermark (rt, NS);
}

static void
reset_fake (size_t cap)
{
  free (fake_alsa.got);
  memset (&fake_alsa, 0, sizeof fake_alsa);
  fake_alsa.cap_frames = cap;
  fake_alsa.got = calloc (cap, sizeof (int16_t));
  fake_alsa.recover_ok = 1;
}

/* Poll through the product API only: the fake's own fields belong to the backend
 * thread until ody_backend_stop() has joined it. */
static int
wait_for (const ody_backend_t *b, uint64_t frames)
{
  ody_backend_stats_t st;
  int i;
  for (i = 0; i < 2000; i++)
    {
      ody_backend_get_stats (b, &st);
      if (st.frames_written >= frames) return 1;
      msleep (5);
    }
  return 0;
}

/* The frames the fake device received must equal what an identical engine
 * renders offline, period by period: no loss, no repeat, no reorder. */
static void
compare_with_reference (size_t got, const char *name)
{
  ody_rt_t ref;
  size_t periods = (got + PERIOD - 1) / PERIOD, p;
  int16_t *want = calloc (periods * PERIOD, sizeof (int16_t));
  fill (&ref);
  for (p = 0; p < periods; p++)
    ody_rt_render (&ref, want + p * PERIOD, PERIOD, 1);
  CHECK (got > 0 && memcmp (want, fake_alsa.got, got * sizeof (int16_t)) == 0,
         "%s: the %zu frames the device received equal the rendered stream exactly", name, got);
  free (want);
  ody_rt_destroy (&ref);
}

static ody_backend_t *
make (ody_rt_t *rt, const char *dev, char *err, size_t el)
{
  ody_backend_cfg_t c = { dev, RATE, PERIOD, 1 };
  return ody_backend_create ("alsa", rt, &c, err, el);
}

int
main (void)
{
  ody_rt_t rt;
  ody_backend_t *b;
  ody_backend_stats_t st;
  char err[256];

  /* --- configuration and open errors --- */
  fill (&rt);
  reset_fake (1000);
  fake_alsa.open_error = -ENOENT;
  err[0] = 0;
  b = make (&rt, "hw:9", err, sizeof err);
  CHECK (b == NULL && strstr (err, "hw:9") && strstr (err, "fake: no such device"), "open error names the device and the reason: '%s'", err);
  reset_fake (1000);
  fake_alsa.params_error = -EINVAL;
  b = make (&rt, NULL, err, sizeof err);
  CHECK (b == NULL && strstr (err, "48000 Hz") && strstr (err, "fake: invalid argument"), "set_params error reports rate and reason: '%s'", err);
  CHECK (fake_alsa.close_calls == 1, "a failed configure closes the opened device (%d)", fake_alsa.close_calls);
  reset_fake (1000);
  b = make (&rt, NULL, err, sizeof err);
  CHECK (b != NULL, "creates with the default device");
  CHECK (strcmp (fake_alsa.last_name, "default") == 0 && fake_alsa.rate == RATE && fake_alsa.channels == 1 &&
         fake_alsa.soft_resample == 0, "opened 'default', 48000 Hz, 1 ch, no soft resample");
  ody_backend_destroy (b);
  CHECK (fake_alsa.drain_calls == 1 && fake_alsa.close_calls == 1, "destroy drains then closes once");
  ody_rt_destroy (&rt);

  /* --- partial writes and xruns in the middle of periods --- */
  fill (&rt);
  reset_fake (40000);
  fake_alsa.max_chunk = 100;
  fake_alsa.n_xrun = 3;
  fake_alsa.xrun_at[0] = 5000;      /* mid-period (5000 = 10*480 + 200) */
  fake_alsa.xrun_at[1] = 5000;      /* an immediate second failure with no progress */
  fake_alsa.xrun_at[2] = 12345;
  b = make (&rt, NULL, err, sizeof err);
  CHECK (b != NULL && ody_backend_start (b) == 0, "start");
  CHECK (wait_for (b, 20000), "device received 20000 frames in time");
  ody_backend_stop (b);
  ody_backend_get_stats (b, &st);
  CHECK (st.fatal == 0, "no fatal error");
  CHECK (st.xruns == 3 && st.recoveries == 3 && fake_alsa.recover_calls == 3, "3 xruns, 3 recoveries (%llu,%llu)", (unsigned long long) st.xruns, (unsigned long long) st.recoveries);
  CHECK (st.partial_writes > 100, "partial writes counted: %llu", (unsigned long long) st.partial_writes);
  CHECK (st.periods > 0 && st.frames_written == fake_alsa.got_frames, "frames_written %llu == device %zu", (unsigned long long) st.frames_written, fake_alsa.got_frames);
  compare_with_reference (fake_alsa.got_frames, "xruns+partials");
  ody_backend_destroy (b);
  ody_rt_destroy (&rt);

  /* --- a device that dies: bounded, reported, no spin, clean shutdown --- */
  fill (&rt);
  reset_fake (40000);
  fake_alsa.dead_after = 3000;
  fake_alsa.recover_ok = 0;
  b = make (&rt, NULL, err, sizeof err);
  CHECK (b != NULL && ody_backend_start (b) == 0, "start (dying device)");
  {
    int i;
    for (i = 0; i < 1000; i++)
      {
        ody_backend_get_stats (b, &st);
        if (st.fatal) break;
        msleep (10);
      }
  }
  CHECK (st.fatal == EIO, "fatal EIO reported (%d)", st.fatal);
  CHECK (st.recovery_failures >= 30, "recovery failures counted: %llu", (unsigned long long) st.recovery_failures);
  {
    uint64_t p0 = st.periods;
    msleep (100);
    ody_backend_get_stats (b, &st);
    CHECK (st.periods == p0, "after giving up the thread no longer renders (%llu -> %llu)", (unsigned long long) p0, (unsigned long long) st.periods);
  }
  ody_backend_stop (b);               /* must not hang; also joins the thread before we read the fake */
  CHECK (fake_alsa.got_frames == 3000, "exactly the frames before the failure were delivered (%zu)", fake_alsa.got_frames);
  compare_with_reference (fake_alsa.got_frames, "dying device");
  ody_backend_destroy (b);
  ody_rt_destroy (&rt);

  free (fake_alsa.got);
  printf ("test_alsa_fake: %ld checks, %ld failures\n", checks, failures);
  return failures ? 1 : 0;
}
