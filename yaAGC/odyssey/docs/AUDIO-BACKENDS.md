# Plan: a cross-platform real audio backend

ODYSSEY EXTENSION. **This is a plan. Nothing in this file is implemented or
tested.** Statements about third-party libraries are from general knowledge and
are marked *verify* where they must be checked against the vendored version
before relying on them.

## What exists today

| Backend | What it is | Platforms | Status |
|---|---|---|---|
| offline (`--audio-wav`) | the scheduler thread renders the frames due after every round into a WAV file | any | tested, deterministic |
| paced (`--audio-backend paced`) | a thread that *emulates* a device: calls `ody_rt_render` every period against `CLOCK_MONOTONIC` and writes a WAV file. **Not audio output**; it exists to test callback timing and threading without hardware | POSIX | tested |
| alsa (`ODY_WITH_ALSA=1`) | blocking `snd_pcm_writei` loop on its own thread | Linux only | write/recover logic tested with scripted fakes; the real thread tested against a **fake** libasound; **never run against the real libasound or a sound card** |

So the project cannot yet make sound on Windows or macOS, and has never been
heard on Linux either.

## Requirements a real backend must meet (derived from the engine contract)

1. The device thread/callback calls **only** `ody_rt_render` (plus its own
   bookkeeping). It never calls the scheduler, an AGC, the bus, or allocates.
2. `ody_rt_render` accepts any frame count, so a callback with variable or
   odd-sized buffers is fine. Output is 16-bit, 1 or 2 channels (mono mix
   duplicated).
3. The engine's `device_rate` must equal the rate at which the device really
   consumes frames, because timed mode maps frame index to AGC time. Either the
   backend runs the device at that rate, or the library resamples between the
   engine rate and the hardware rate (this is playback-format conversion, not
   synthesis, and must be documented as such).
4. Device loss, route changes and suspend/resume must be *reported* (a fatal flag
   like `ody_backend_stats_t.fatal`), never silent, and never spin the CPU.
5. It must not generate audio. Libraries that ship tone/noise generators must
   have them compiled out (see below).
6. Start/stop must not leave stale audio queued (needs the consumer-side flush
   noted under "Known gaps" in `AUDIO-ENGINE.md`).

## Candidates

| | miniaudio | PortAudio | SDL2 audio | keep ALSA + write others |
|---|---|---|---|---|
| Distribution | one C header, vendored | system library or separate build | system library | one per OS |
| Licence (*verify*) | public domain / MIT-0 | MIT-style | zlib | n/a |
| Windows | WASAPI (also DirectSound, WinMM) | WASAPI, DirectSound, ASIO (extra SDK) | yes | write WASAPI by hand |
| macOS | Core Audio | Core Audio | yes | write by hand |
| Linux | ALSA, PulseAudio, JACK, (PipeWire via Pulse) | ALSA, JACK, Pulse | via ALSA/Pulse | have ALSA |
| Model | callback on the library's audio thread | callback | callback | blocking write |
| Null device for CI | yes (*verify*) | no | dummy driver | fake |
| Extra build dependency | none (links `dl`, `pthread`, `m` on Linux) | libportaudio on every target | SDL2 (large) | none |

**Recommendation: miniaudio**, vendored as `third_party/miniaudio/` with its
licence file, for these reasons: no system dependency to install on Windows,
the callback model maps directly onto `ody_rt_render`, and it has a null
backend that lets the callback path run in CI without hardware. PortAudio is the
reasonable alternative if vendoring a very large single-header library is unwanted; its callback
model is the same, so the integration below would carry over. SDL2 is more than
is needed.

### Must be compiled out (AGENTS.md: the host never generates oscillators)

miniaudio ships waveform and noise generators. Build with
`MA_NO_GENERATION` (*verify the macro name in the vendored version*) and also
`MA_NO_DECODING`, `MA_NO_ENCODING`, `MA_NO_RESOURCE_MANAGER`, `MA_NO_NODE_GRAPH`,
`MA_NO_ENGINE`. Add a test that greps the final binary's symbols for `ma_waveform`
and `ma_noise` so the rule is enforced rather than remembered.

## Integration design

New backend kind `miniaudio` in `odyssey_backend.c`, compiled only with
`ODY_WITH_MINIAUDIO=1` (default build unchanged, as with ALSA).

```
miniaudio audio thread --data callback--> ody_rt_render(rt, pOutput, frameCount, channels)
```

* Config: `ma_format_s16`, `channels` from `--audio-channels`, `sampleRate` from
  `--audio-rate`, `periodSizeInFrames` from `--audio-period`,
  `performanceProfile = low_latency`. If the device cannot run at that rate
  miniaudio converts; the backend reports whether the device's native rate
  differs.
* **Rate negotiation order:** open the device first, read the rate it actually
  runs at, then `ody_rt_init` with that rate, rather than guessing. (Today
  `ody_rt_init` happens before the backend exists; the CLI flow needs a small
  reorder for this backend.)
* Callback body: `ody_rt_render` plus the same `periods` / `max_render_ns`
  accounting as the other backends. No miniaudio call inside it.
* Notifications (stopped, rerouted, interruption started/ended) go through
  `ma_device_set_notification_callback`; they set counters and the `fatal` flag.
  Unlike ALSA there is no per-period xrun result, so underruns are known only
  through the engine's own counters (which is already the contract).
* `--audio-device NAME` selects by name from `ma_context_get_devices`; add
  `--audio-list-devices`.
* Thread priority and period sizing are left to miniaudio; the engine's
  `--audio-latency-ms` (default 50) stays the cushion against *scheduler* jitter.

## Test plan

Without hardware (CI, can be claimed once written and run):

1. miniaudio **null** backend: callback runs on the library thread; render
   output equals the offline reference for the same engine state; stats and
   `fatal` plumbing; clean stop/destroy; no generator symbols in the binary.
2. The offline renderer and all existing tests unchanged; ASan/UBSan and TSan
   (TSan needs gcc) over the callback path.

With hardware (manual; **nothing may be claimed until someone has done it**):

| Platform | Device | Check |
|---|---|---|
| Linux, PipeWire/Pulse and raw ALSA | speakers | audible, correct pitch relative to the WAV render, 10 minutes with 0 underruns |
| Windows 10/11, WASAPI shared | speakers | same |
| macOS, Core Audio | speakers | same |
| each | 44.1 / 48 / 96 kHz devices | pitch unchanged (rate conversion correct) |
| each | unplug / replug, sleep / resume | reported, no hang, no spin, recoverable by stop/start |
| each | `--audio-period` 64 .. 2048 | worst-callback time well inside the period |

## Portability items found while planning

* `odyssey_backend.c` (paced) uses `clock_nanosleep` and POSIX threads; neither is
  guaranteed on Windows. On MSYS2/MinGW-w64 gcc, `<stdatomic.h>`, `_Alignas` and
  winpthreads are expected to work but `clock_nanosleep` availability is
  *unverified*; the paced backend would need a guard or a Windows timer.
* MSVC: C11 atomics support is recent and flag-gated (*verify*); the first Windows
  target should be MinGW-w64 gcc, not MSVC.
* The disassembly check in `tests/run_tests.sh` (no calls inside `ody_rt_render`)
  is x86-64 gcc specific; on other targets it must be re-run or skipped with a
  stated reason.
* `ody_rt_init` already refuses to run (`-3`) where the atomics it needs are not
  lock-free, so an unsupported 32-bit target fails loudly.
* 64-bit atomics in the engine (watermark, counters) are the only items that can
  be non-lock-free on 32-bit ARM/MIPS; `_Static_assert`s in `odyssey_rt.h` stop
  the build there.

## Order of work

1. Vendor miniaudio, add the compile-out flags and the symbol test.
2. Null-backend callback test (step 1 above). Exit: tests pass, existing suite
   unchanged.
3. Reorder CLI start-up for device-first rate negotiation.
4. Real-device runs on Linux, then Windows, then macOS, recording results in
   `AUDIO-ENGINE.md`.
5. Only then decide whether the ALSA backend is kept (it adds little once
   miniaudio is verified on Linux) or retired.
