# Milestone 8, phase 1: real-time audio engine

ODYSSEY EXTENSION. Phase 2 (the GUI) is **not started**; this document describes
the engine as reviewed and corrected before any GUI work (see "Phase 1 review"
at the end). Nothing here
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
| Scheduler (producer) | `ody_rt_push`, `ody_rt_sink`, `ody_rt_set_active`, `ody_rt_set_watermark` / `publish_watermark`, and the scheduler/computers/bus (none of which are thread-safe) |
| Audio callback (consumer) | `ody_rt_render` only (offline mode: the scheduler thread plays both roles in turn) |
| Any other thread (GUI/CLI) | the control and monitoring API listed under "Control and monitoring API" and nothing else |

Not thread-safe, by design: the scheduler, the computers, the capture queues
(`ody_sample_queue_t`) and the bus. Only the SPSC PCM rings and the atomics
documented here are shared between threads. Anything a UI needs from the
scheduler side has to be published by the scheduler thread; a UI must never call
`odyssey_*` functions directly.

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
* The **watermark** is stored with release *after* the pushes and loaded with
  acquire once at the start of `ody_rt_render`; every sample stamped at or below
  it is therefore visible to the callback. Samples newer than the loaded
  watermark that are already in the ring are gated by the stamp comparison, so a
  stale watermark only makes the callback conservative.
* Counters and control values (gain, mute, solo, active, prefill, master, peak)
  are independent atomics accessed **relaxed**: the callback only needs *some*
  recent value and never relies on ordering against other data. Each counter has
  exactly one writer (producer-side or consumer-side), so no value is lost.
* Non-atomic configuration (`timed`, `latency_cycles`, `device_rate`, `stream_rate`)
  is written before the audio thread starts and never after; thread creation
  orders it. Changing it while a callback runs is not supported.
* Peak meter: the callback does load-then-store (no CAS retry loop) and
  `ody_rt_read_peak` does an exchange. A race between them can make the reader see
  one period's peak twice; it can never produce a value that was not a real peak.
* `ody_rt_get_stats().fill` is read as tail then head and clamped to the capacity;
  it is an estimate that may overshoot by the samples consumed in between, but is
  never out of range.

## Lock-free requirement

Every atomic the callback touches must be lock-free, otherwise the C library may
substitute a lock inside the callback. This is enforced twice: `_Static_assert`s in
`odyssey_rt.h` on `ATOMIC_INT_LOCK_FREE`, `ATOMIC_LONG_LOCK_FREE` and
`ATOMIC_LLONG_LOCK_FREE` (the build fails on a target that cannot), and a runtime
`atomic_is_lock_free` check in `ody_rt_init` on the real objects (returns `-3`).
`test_rt` also asserts it. The 64-bit atomics are the ones at risk on 32-bit
targets. Checked here only on x86-64 gcc/clang; other targets are unverified.
* `ody_rt_render` snapshots every control once per call, so one buffer is
  internally coherent; a change takes effect on the next period.

## Audio callback constraints (`ody_rt_render`)

No allocation, no locks, no system calls, no stdio, no AGC instructions;
work is a fixed loop over `frames x 2 channels` with a bounded inner pop (at most
256 pops per frame). The callback never waits for another thread: it is
**lock-free and non-blocking with bounded work per call**. The ring push/pop each
take a fixed number of steps with no retry loop. This document does **not** claim
wait-freedom of `ody_rt_render` as a whole: counter updates use `fetch_add`, which
x86-64 compiles to one `lock`-prefixed instruction but which is an LL/SC retry
loop on some other architectures (lock-free, not strictly wait-free). Nothing here
is a hard real-time guarantee; the bound on time is measured, not proven. Verified by: code review; `tests/run_tests.sh` disassembles `ody_rt_render` and
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
* **Peak meters:** per channel, after the channel gain and before mute and master
  (so a muted channel still shows its level and master does not move it).
  `ody_rt_read_peak` reads and resets; `ody_rt_get_stats().peak` reads without
  resetting. There is no mix-level peak; the post-master clip count
  (`ody_rt_clipped_frames`) is the overload indicator.

## Backends

| Backend | Status |
|---|---|
| offline (`--audio-wav F`) | deterministic: the scheduler renders exactly the frames due for the AGC time elapsed after every round; byte-identical repeat runs |
| paced (`--audio-backend paced --audio-device F.wav`) | real thread calling `ody_rt_render` every period on a wall-clock (CLOCK_MONOTONIC absolute deadlines); file I/O is in the emulator thread, not in the callback. Tested. Implies `--realtime` (scheduler paced to wall clock). |
| alsa (`make ODY_WITH_ALSA=1`, `--audio-backend alsa --audio-device NAME`) | **Never run against the real libasound or a sound device** (the sandbox has neither). Logic tested here: `ody_pcm_write_all` against scripted fake devices, and the real `alsa_thread` / open / configure code against a **fake** libasound (`tests/fake_alsa`, whose prototypes are written from the documented libasound API, not from the real header). Device (`--audio-device`), rate (`--audio-rate`) and period (`--audio-period`) are wired; ALSA may refuse a rate/period (open errors name the device and the reason; the CLI exits 6). |

### ALSA write and recovery behaviour (tested against fakes)

`ody_pcm_write_all` delivers every frame of each rendered period, in order, exactly
once. A **partial write** continues with the remainder. After an error (`-EPIPE`
xrun, `-ESTRPIPE` suspend, `-EINTR`, ...) it calls `snd_pcm_recover` and then
retries the **remainder**: nothing in the period is skipped or repeated. `-EAGAIN`
and zero-frame returns are waits (1 ms), not spins. After 32 consecutive errors
with no progress (256 consecutive waits) it gives up, sets `fatal` (the last
errno) and the thread stops rendering; `ody_backend_get_stats` and the CLI
(`audio_device_*` lines, and a message on stderr) report it. Counters: partial
writes, xruns, write errors, recoveries, recovery failures, stalls, frames
written, frames abandoned at stop.

Consequences that are accepted and documented rather than hidden: an xrun stall
becomes **extra latency** (the late period is still played, not discarded), and
while the thread is stalled the rings are not drained, so a long stall shows up as
ring overruns and, in timed mode, as the scheduler having raced ahead. There is no
realtime thread priority and no ALSA `avail`/delay accounting. `soft_resample` is
0 on purpose: a device that cannot run the exact rate fails to open instead of
being resampled behind the engine's back.

## Reset semantics (queued audio)

What resetting **one** computer (`odyssey_scheduler_reset_computer`) does to **its**
audio, decided and tested (`tests/test_rt_reset.c`):

| Thing | Behaviour |
|---|---|
| Samples already in the ring | **Play out normally.** They are audio the computer really produced; the ring is not flushed (the producer must never move the consumer's `tail`). |
| Stamps of later samples | Delivered to the audio sink on the **scheduler timeline**: `reset time + the computer's new local CycleCounter`. The computer's own CycleCounter restarts at 0, so without this the stamps ran backwards, the ring's head-of-line stamp blocked, and the new stream was replayed in a compressed burst. (This was a real bug in the first Phase 1; found and fixed in the review.) CSV logs still carry the computer's own CycleCounter. |
| Output level at the reset | One **rest sample** (`ODY_SEQ_REST`, PCM 0) is delivered at the reset time: the AGC channel registers are zero after power-on, so the held output level returns to 0 instead of freezing at the last pre-reset value. It is the AGC's own power-on state, not a waveform. If the ring is full at that moment it is dropped and counted like any other drop. |
| The other computer | Untouched: same samples, stamps, audio (tested frame for frame). |
| Mixer (gain, mute, solo, master) | Untouched: these belong to the user, not the AGC. |
| Counters (pushed, dropped, underruns, overruns) and peak | Untouched: they describe the engine's lifetime, not one AGC run. |
| Sequence numbers | Restart at 0 (as before); `ody_capture_reset` clears the validation state. |
| When | Only on the scheduler thread, between rounds (the scheduler owns the computers). |

Not provided: a **hard flush** of a ring while the audio thread runs (for a global
stop/restart). That needs a handshake owned by the consumer; it does not exist in
Phase 1 and is listed under Known gaps.

## Control and monitoring API (the contract for any UI)

A UI/CLI thread may call **only** these. Each is a single atomic operation; none
blocks, allocates or calls into the scheduler.

| Purpose | Calls |
|---|---|
| Per-channel gain | `ody_rt_set_gain_q15(rt,id,q15)` (Q15, 32768 = 1.0, clamped to 4.0), `ody_rt_gain_from_float(g)`, `ody_rt_get_gain_q15` |
| Mute / solo | `ody_rt_set_mute`, `ody_rt_set_solo`, `ody_rt_get_mute`, `ody_rt_get_solo` (mute wins over solo) |
| Master gain | `ody_rt_set_master_q15`, `ody_rt_get_master_q15` |
| Per-channel statistics | `ody_rt_get_stats(rt,id,&st)`: `pushed dropped overrun_events consumed underrun_events underrun_frames fill peak` |
| Peak meter | `ody_rt_read_peak(rt,id)` (read and reset, range 0..32767) |
| Engine statistics | `ody_rt_rendered_frames`, `ody_rt_clipped_frames` |
| Backend health (backend object) | `ody_backend_get_stats`: periods, worst callback ns, device frames written, partial writes, xruns, recoveries, errors, stalls, `fatal` |

`id` is `ODY_COMPUTER_CMC` (0) or `ODY_COMPUTER_LGC` (1); out-of-range ids are
ignored and read as 0. Rules:

* **Never access a ring**, `rt->chan[...]`, `slots`, `head` or `tail`, and never call
  `ody_rt_push*`, `ody_rt_sink`, `ody_rt_render`, `ody_rt_set_active`,
  `ody_rt_set_watermark`, `ody_rt_set_prefill`, `ody_rt_set_latency_cycles`,
  `ody_rt_init` or `ody_rt_destroy`. `tests/check_gui_api.sh` enforces this on
  GUI-facing sources (`odyssey_gui*`, `odyssey_dsky*`, `gui/*`); its `--selftest`
  proves it flags each kind of violation. (The structs are visible only because the
  engine embeds them in caller-owned memory.)
* **Consistency:** each value is individually atomic, but a set of values is not a
  snapshot. `pushed`, `consumed` and `fill` read together can disagree by the
  operations in flight, so a UI must not assert `pushed == consumed + fill` on a
  live engine. Setting several controls is not atomic as a group either; a change
  takes effect at the next audio period.
* **Peak meters have one reader.** `ody_rt_read_peak` resets, so two readers would
  steal each other's peaks; a UI wanting several views should read once per poll
  and fan out. Poll at the display rate (tens of Hz); polling faster only reads
  smaller windows.
* **Lifetime:** the UI must not outlive `ody_rt_destroy`; stop the UI thread (or its
  polling) first.
* **Scheduler-side data** (DSKY state, bus counters, computer state) is not part of
  this API and is not thread-safe. A GUI needs the scheduler thread to publish a
  snapshot and to execute commands; that mechanism is Phase 2 design work and does
  not exist yet.
* **Offline determinism** is unaffected: offline runs have no UI thread, and the
  control functions only change gain/mute/solo/master, which are inputs to the
  render just like the CLI options.

## Test results (this sandbox)

`make test`: 281 PASS / 0 FAIL (M1-M7 and every earlier Phase 1 check still pass
unchanged, isolation suite passes). `make asan`: 279 PASS (the disassembly check is
skipped under sanitizers), only the documented pre-existing upstream `DAS` UBSan
finding. `test_rt`: 16499 checks, 0 failures; `test_rt_reset`: 10164 checks;
`test_alsa_fake`: 19 checks. `make tsan` (gcc: `test_rt` and the threaded
`test_alsa_fake`) clean on three consecutive runs.

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

* ALSA backend has never run against the real libasound or a sound device; no other
  real-device backend exists (plan: `AUDIO-BACKENDS.md`). A device whose native rate
  is not 48 kHz needs `--audio-rate`. The paced backend is a WAV-writing timing
  emulator, not audio output.
* Reset of one computer: see "Reset semantics". There is no hard flush of a ring
  while the audio thread runs (needed for a global stop/restart); it needs a
  consumer-owned handshake that does not exist yet.
* Timed mode assumes a producer that publishes watermarks (the scheduler does; a
  custom feeder must call `ody_rt_set_watermark` after its pushes).
* Zero-order hold (no interpolation or band-limiting): audible aliasing is expected;
  that is a representation choice, not synthesis.
* The scheduler, computers and bus are single-threaded and not thread-safe; only the
  rings/controls/counters are cross-thread. A UI needs scheduler-published
  snapshots and a command path that do not exist yet.
* No real-time guarantee: lock-free and non-blocking with bounded work, measured
  (not proven) callback time, no thread priority.
* Wall-clock tests (paced section) can in principle flake under extreme machine load.
* Mono mix only (no pan); peak meters are pre-mute.

## Phase 1 review (before any GUI work)

A review pass over Phase 1 found and changed the following; nothing else was
rewritten.

| Item | Finding | Change | Evidence |
|---|---|---|---|
| Ring ordering | Correct as designed (push: tail acquire / head release; pop mirrored; watermark release-after-push, acquire-at-start). Not previously written down for the watermark, config fields or the peak race. | Documented ("Ring memory ordering") | TSan clean x3 |
| Lock-free | Assumed, never checked; 64-bit atomics can lock on 32-bit targets | `_Static_assert`s + runtime `atomic_is_lock_free` in `ody_rt_init` (`-3`) | `test_rt` |
| "wait-free" | Over-claimed (`fetch_add` is an LL/SC loop on some CPUs) | Reworded to lock-free / non-blocking / bounded work, no hard real-time claim | this file |
| `get_stats().fill` | Tail-then-head read could overshoot capacity under concurrency | Clamped to 0..capacity | monitor thread in `run_stream` |
| ALSA write | One `snd_pcm_writei` per period: a partial write lost the remainder; after recovery the whole period was lost; a failing recover spun the CPU | `ody_pcm_write_all`: remainder retried, bounded give-up with `fatal`, counters, better open errors | scripted fakes + fake libasound; the original behaviour fails 7 checks |
| Reset | After a one-computer reset the stamps ran backwards (CycleCounter restarts at 0): timed playback of the reset computer was wrong (reproduced: 8 failing checks) | Stamps rebased onto the scheduler timeline + rest sample ("Reset semantics") | `test_rt_reset`; two mutants caught |
| Default `make` | The first rule from `build/*.d` became the default goal, so a plain `make` after a build silently built almost nothing | `.DEFAULT_GOAL := all` | observed and fixed |
| Docs | Stale M8 status and single-thread statements | Corrected in HANDOFF, SYNTH-PROTOCOL, BUS-PROTOCOL, `odyssey_audio.h` | review |
| UI contract | No way to read mixer state back; boundary unenforced | `ody_rt_get_gain_q15/mute/solo/master_q15`, "Control and monitoring API", `tests/check_gui_api.sh` | `test_rt`; checker selftest |
| Backend plan | Only ALSA existed | `AUDIO-BACKENDS.md` (plan only, nothing implemented) | n/a |

Not changed on purpose: the offline renderer and its determinism tests (all pass
unchanged), the mixer arithmetic, the timed/fixed modes.
