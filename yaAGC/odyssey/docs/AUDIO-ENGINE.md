# Milestone 8, phase 1: real-time audio engine

ODYSSEY EXTENSION. Phase 2 (the DSKY GUI) is **not started**. Nothing here
generates a waveform: the host converts the AGC word to PCM16, holds it to the
device rate, scales and sums. All oscillator/noise values still come from the
AGC programs.

## Data path

```
CMC AGC --0300--> sample queue --capture drain--> sink --> SPSC ring (PCM16) --\
                                                                               >-- ody_rt_render() --> device/WAV
LGC AGC --0300--> sample queue --capture drain--> sink --> SPSC ring (PCM16) --/
   (scheduler thread = PRODUCER)                                 (audio callback = CONSUMER)
```

`odyssey_rt.[ch]` ring + mixer + render; `odyssey_backend.[ch]` WAV writer,
offline pump, paced device emulator, optional ALSA; `odyssey_capture` got a
per-sample `sink` hook and the scheduler a `set_sink` / `set_round_hook`
(no behaviour change when unset).

## Thread roles (hard rules)

| Thread | May call |
|---|---|
| Scheduler (producer) | `ody_rt_push`, `ody_rt_sink`, `ody_rt_set_active` |
| Audio callback (consumer) | `ody_rt_render` only (offline mode: the scheduler thread plays both roles in turn) |
| Any other thread (GUI/CLI) | all `ody_rt_set_*`, `ody_rt_get_*`, `ody_rt_read_peak` |

## Ring memory ordering (C11 atomics)

Single-producer/single-consumer, `head`/`tail` free-running `uint32_t`, capacity
a power of two, each index on its own cache line.

* push: `head` relaxed (own), `tail` **acquire** (pairs with pop's release, so the
  consumer has finished reading a slot before it is overwritten), plain slot
  store, `head` **release** store (publishes the slot).
* pop: `tail` relaxed (own), `head` **acquire** (sees the slot write), plain slot
  load, `tail` **release** store (returns the slot).
* Fullness = `head - tail >= capacity`; correct across the 2^32 wrap (tested by
  starting indices at 0xFFFFFFF9).
* Counters and control values (gain, mute, solo, active, prefill, master, peak)
  are independent atomics accessed **relaxed**: the callback only needs *some*
  recent value and never relies on ordering against other data.
* `ody_rt_render` snapshots every control once per call, so one buffer is
  internally coherent; a change takes effect on the next period.

## Audio callback constraints (`ody_rt_render`)

No allocation, no locks, no system calls, no stdio, no AGC instructions;
work is a fixed loop over `frames x 2 channels` with a bounded inner pop. Atomic
read-modify-writes compile to single `lock`-prefixed instructions (wait-free).
Verified by: code review; `tests/run_tests.sh` disassembles `ody_rt_render` and
requires that it contains no call except the compiler's `__stack_chk_fail`
abort path; measured render time (below).

## Playback timing: two modes

* **TIMED (default, `--audio-stream-rate 0`).** Every ring slot carries the
  sample's AGC cycle stamp. Device frame `f` plays AGC time
  `T(f) = floor(f * 85333 / device_rate) - latency`, holding the newest sample
  stamped `<= T`. Both computers share this one AGC clock, so there is no
  sample-rate constant to guess and the CMC and LGC cannot drift against each
  other. The producer publishes a per-computer **watermark** (all samples up to
  AGC cycle W are queued; stored with release *after* the pushes). A frame whose
  `T > W` is an underrun (the AGC has not produced that moment yet); samples
  already delivered keep playing, the last one is held, and when the producer
  catches up playback resumes at the correct AGC time (late samples are skipped,
  never played late). Frames with `T < 0` (the latency lead-in) are silent and
  not counted. At most 256 samples are consumed per frame, bounding catch-up
  work. `--audio-latency-ms` (default 50) must exceed the scheduler's jitter:
  measured in the sandbox with the paced scheduler, latency >= 10 ms gave zero
  underruns for 11 ms, 93 ms and 468 ms scheduler slices; latency 0 starved
  (the wall clock overtakes each round's watermark before the next round
  publishes). The offline renderer renders exactly the frames whose `T <= W`
  after each round, so offline output is deterministic and slice-independent in
  timing.
* **FIXED (`--audio-stream-rate N`, N > 0).** The next AGC sample is taken every
  `device_rate/N` frames with an exact Q32 accumulator; cycle stamps are
  ignored. Kept for compatibility and as a simple model; it needs N below the
  AGC's real output rate (about 950-990 samples per AGC-second in the synth ROMs)
  or prefill, otherwise the stream outruns the producer and starves.
* **Mix:** per-channel Q15 gain (0..4.0, round-to-nearest) clamped to +-32767;
  `audible = !mute && (no channel soloed || this channel soloed)`, so **mute
  always wins over solo**; summed; Q15 master (0..4.0); saturating clip to
  +-32767 (counted in `clipped_frames`, never wraps). Mono mix is copied to
  both output channels (or one channel with `--audio-channels 1`).
* **Underrun** (fixed mode) = an *active*, primed channel needed its next stream
  sample and the ring was empty; (timed mode) see above. The last value is held (no click burst); every such device
  frame is counted in `underrun_frames`, each episode in `underrun_events`,
  independently for CMC and LGC. No backlog is accumulated (the stream resumes
  at its own pace, no fast-forward). An inactive channel never counts.
* **Prefill** (`--audio-prefill N`, fixed mode only): a channel stays silent, uncounted, until N
  samples are queued; builds a latency cushion.
* **Overrun** = the ring was full; the *newest* sample is dropped (`dropped`),
  each full episode counted in `overrun_events`.
* **Peak meters:** post-gain, pre-mute per channel; `ody_rt_read_peak` reads and
  resets (the GUI will use these).

## Backends

| Backend | Status |
|---|---|
| offline (`--audio-wav F`) | deterministic: the scheduler renders exactly the frames due for the AGC time elapsed after every round; byte-identical repeat runs |
| paced (`--audio-backend paced --audio-device F.wav`) | real thread calling `ody_rt_render` every period on a wall-clock (CLOCK_MONOTONIC absolute deadlines); file I/O is in the emulator thread, not in the callback. Tested. Implies `--realtime` (scheduler paced to wall clock). |
| alsa (`make ODY_WITH_ALSA=1`, `--audio-backend alsa --audio-device NAME`) | **UNTESTED**. The sandbox has no ALSA library or sound device. Only compile-checked against a stub header with the real ALSA prototypes. Device selection (`--audio-device`), rate (`--audio-rate`) and period (`--audio-period`) are wired; ALSA may refuse a rate/period (the call fails with exit 6). |

## Test results (this sandbox)

`make test`: 274 PASS / 0 FAIL (M1-M7 all still pass, isolation suite passes).
`test_rt`: 16416 checks, 0 failures (also under ASan+UBSan and ThreadSanitizer
with gcc: `make tsan`), repeated runs stable.

Required demonstrations: (1) CMC alone, (2) LGC alone, (3) both, (4) independent
gains, (5) mute/solo incl. both, (6) master: offline WAV from the real SYNTH
ROMs equals an independent Python model (`tests/oracle_rt.py`) exactly, 168750
frames each, 0 mismatches; unit tests check 16 control combinations x mono/stereo
against a double-precision model. (7) CMC-only + LGC-only equals the mix frame for
frame, on real AGC streams and on distinct synthetic patterns. (8) simulated
underruns exact (counts and episodes), and actual underruns from a real callback
thread over an unfed ring (`rendered - 48` frames starve: first tick). (9) three
sustained 3,000,000-sample producer/consumer runs on real threads (ring 64 and
4096, blocking and dropping producers): no reorder/duplication, dropped
samples equal the gaps exactly, pushed == consumed + queued. (10) existing tests.

Measured, not guaranteed: `ody_rt_render(512 frames)` mean about 4 us, worst
about 0.1-0.8 ms in this sandbox (including under TSan, with scheduling noise);
period budget at 48 kHz/512 is 10.7 ms. Paced real-time runs: 2.0 AGC-seconds in
2.01 s wall, 0 underruns/drops (timed, latency 50 ms; and fixed stream rate 500 with prefill 200).

Timed mode: the WAV rendered from the real SYNTH ROMs equals an independent
model exactly at scheduler slices 1/7/1000/4096 and latencies 0-100 ms (all gain,
mute, solo, master, clip, mono and stereo variants); byte-identical on repeat.
A bug found and fixed while testing it: the first version stopped consuming
while starved, which deadlocks a producer blocked on a full ring; the callback
now always drains samples at or below `min(T, watermark)`.
Mutation testing of the timed path: 7 more injected bugs (latency ignored,
starved drain, newer-than-T pops, `>=` underrun test, frames_ready off by one,
wrong clock, frame counter not advanced) are all detected (two by hanging, so
`test_rt` now has a watchdog alarm).

Mutation testing (fixed-stream path): 11 injected bugs (mute/solo, master, clipping, gain index,
underrun accounting, ring-full handling, overrun episodes, inactive channels,
backlog fast-forward, prefill); all caught after one missing test (backlog) was
added. One mutant was equivalent and discarded.

## Known limitations

* ALSA backend untested; no other real-device backends (PortAudio/CoreAudio/WASAPI).
  A device whose native rate is not 48 kHz needs `--audio-rate`.
* Resetting one computer does not flush its ring (queued samples play out).
* Timed mode assumes a producer that publishes watermarks (the scheduler does; a
  custom feeder must call `ody_rt_set_watermark` after its pushes).
* Zero-order hold (no interpolation or band-limiting): audible aliasing is expected;
  that is a representation choice, not synthesis.
* The scheduler and bus remain single-threaded; only the ring/controls are cross-thread.
* Wall-clock tests (paced section) can in principle flake under extreme machine load.
* Mono mix only (no pan); peak meters are pre-mute.
