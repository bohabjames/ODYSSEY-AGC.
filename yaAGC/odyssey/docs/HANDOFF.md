# ODYSSEY-AGC handoff (Claude -> Codex)

Branch history: M1-M6 merged to `master` (PRs #1, #2). M7 is on `claude/odyssey-m7`.

## State

Milestones 1-7 of AGENTS.md are done, each with test evidence. **Milestone 8
phase 1 (real-time audio engine) is done, tested and has been reviewed and
corrected (see "Phase 1 review" in `AUDIO-ENGINE.md`); phase 2 (GUI) is NOT
started.** A redesign of the GUI is expected, so build it on the control and
monitoring API in `AUDIO-ENGINE.md`, not on any earlier sketch. No real audio
device has ever been used by this project (ALSA never ran against real libasound;
the paced backend only writes a WAV).

| M | Status | Evidence |
|---|---|---|
| 1 engine runs embedded | done | upstream demo clean, also under ASan/UBSan |
| 2 AGC writes audio words (SEQTEST) | done | exactly 8 expected words, 0 alarm restarts |
| 3 AGC-side synthesis (PHASE-*) | done | AGC output equals independent Python model (about 145k samples); SILENT yields 0 samples |
| 4 state isolation | done | differential vs frozen pristine engine bit-identical; interleaved/reset/destroy isolation; controls fail on the pristine engine |
| 5 dual execution | done | `test_sched` (70 checks) + CLI: dual output equals isolated runs at slices 1, 7, 1000, 4096, 299999; reset/destroy of one leaves the other untouched; mutation-tested |
| 7 dual synthesis | done | `SYNTH-CMC`/`SYNTH-LGC` match an independent exact model (base and ALT); bus-off controls; identical-prefix-then-diverge causality; closed-loop base vs ALT; slices 1/7/4096; deterministic; see `SYNTH-PROTOCOL.md` |
| 8.1 real-time audio | done, reviewed | SPSC atomic rings, mixer, paced backend; WAV equals independent model; unit/stress under ASan+TSan; mutation-tested; reset semantics tested; ALSA logic tested only against fakes (never real libasound/hardware); see `AUDIO-ENGINE.md`, `AUDIO-BACKENDS.md` |
| 6 virtual bus | done | `test_bus` (about 422k checks incl. a 70000-message wrap lap) + BUS-* ROMs using real `READ`/`WRITE`; both directions and duplex; slice-invariant content; mutation-tested (9/9 caught) |

`make test` = 281 PASS lines, 0 FAIL, plus the isolation suite. `make asan` passes
(279 PASS; disassembly check skipped under sanitizers). `make tsan` (gcc only) covers
`test_rt` and the threaded fake-ALSA test.
No upstream VirtualAGC file changed in M5/M6 (only the 3 files from M4).

## Build / test

```
# yaYUL is not built here; build it once from the repo root:
#   cmake -S . -B /tmp/b && cmake --build /tmp/b --target yaYUL
export YAYUL=/tmp/b/yaYUL/yaYUL
make -C yaAGC/odyssey                 # build (header dependencies are tracked)
make -C yaAGC/odyssey test            # unit, scheduler, bus, ROM, isolation tests
make -C yaAGC/odyssey asan            # same under ASan + UBSan
make -C yaAGC/odyssey clean           # NOTE: also deletes assembled ROMs; `make test` rebuilds them
```

Single computer: `build/odyssey-agc --rom R.bin --cycles N --slice S --out s.csv`
Dual: `build/odyssey-agc --cmc-rom C.bin --lgc-rom L.bin [--bus] [--ack-timeout N]
--cycles N --slice S --out-cmc c.csv --out-lgc l.csv` (stats on stdout:
`cmc_*`, `lgc_*`, `rounds`, and with `--bus` `cmc_bus_*` / `lgc_bus_*`).

## Read first

* `BUS-PROTOCOL.md` - the bus contract, scheduler boundary, engine facts, limits
* `AGC-PROGRAMMING-RULES.md` - watchdogs, CCS/TS/one's-complement, register
  addresses, yaYUL gotchas (read before writing any AGC assembly)
* `AUDIO-ENGINE.md` - M8 phase 1: ring ordering, callback rules, reset semantics,
  the UI control/monitoring API, tests, limits, review record
* `AUDIO-BACKENDS.md` - plan (not implemented) for a cross-platform real backend
* `SYNTH-PROTOCOL.md` - M7 programs, message semantics, verification
* `STATE-ISOLATION-AUDIT.md`, `UPSTREAM-CHANGES.md`, `CHANNEL-VERIFICATION.md`

## Code map (yaAGC/odyssey)

`odyssey_engine.[ch]` computer lifecycle; `odyssey_io.c` engine hooks
(`ChannelOutput` routes audio/bus by `State->agc_clientdata`, `ChannelInput`
republishes the mailbox); `odyssey_audio.[ch]` sample queue + conversions;
`odyssey_capture.[ch]` drain/validate/CSV; `odyssey_scheduler.[ch]` round-robin
(round order is a documented contract); `odyssey_bus.[ch]` the bus; `odyssey_rt.[ch]` atomic rings + mixer + render, `odyssey_backend.[ch]` WAV/paced/ALSA backends and the device write loop (`ody_pcm_write_all`); `tests/fake_alsa/` a FAKE libasound for tests; `tests/check_gui_api.sh` GUI/engine boundary check;
`flight/tests/*.agc` test ROMs; `tests/` all tests and the Python oracle.

## Measured (this sandbox; measurements, not guarantees)

* PHASE ROMs: about 496 Hz / 981 Hz / 1917 Hz mean sample rate (targets 500/1k/2k).
* Dual + bus, slice 1000: about 199x real time per computer (both running).
* Bus delivery: one message per round per direction; 12 messages -> last report
  at cycle 12103 (slice 1000), 49251 (slice 4096), 1465 (slice 1).

## Known issues / caveats

* Pre-existing upstream UBSan finding in the `DAS` handler (`c(Address10 - 1)`
  at `Address10 == 0`); identical in the frozen baseline; hit only by the
  Comanche055 isolation workload; left unfixed on purpose.
* Threading: the scheduler, computers, capture queues and bus are single-threaded
  (scheduler thread only). Only the SPSC PCM rings, mixer controls and counters in
  `odyssey_rt` are cross-thread (atomics, TSan-tested). A UI must use only the API in
  `AUDIO-ENGINE.md` and gets scheduler-side data through snapshots the scheduler
  thread publishes (not built yet).
* Resetting one computer: audio stamps are rebased onto the scheduler timeline and a
  rest sample is emitted (`AUDIO-ENGINE.md`, "Reset semantics"). No ring flush while
  the audio thread runs exists yet.
* A plain `make` now builds everything (a `.DEFAULT_GOAL` fix; before, it could
  silently build only one object once `build/*.d` existed).
* Section 3 of `STATE-ISOLATION-AUDIT.md`: debugger, coverage, DEDA, telemetry
  and socket subsystems must stay off with more than one computer.
* `0301` is a record-only status word (M7); the host never acts on it.
* Message semantics are defined in `SYNTH-PROTOCOL.md` for the synth ROMs; the
  M6 BUS-* test ROMs still use opaque patterns. The receiver test ROMs report their results on
  the audio channel 0300 purely so the host can capture them.
* One message per round per direction is deliberate (determinism), so the
  scheduler slice bounds bus throughput (about 85333/slice messages per AGC-second).
* `tests/baseline/` is a frozen copy of pristine engine files; never edit it.

## Next steps

1. M8 phase 2: GUI (redesign pending). It must only call the control/monitoring API
   (`AUDIO-ENGINE.md`; `tests/check_gui_api.sh` enforces it), keep the scheduler off
   the GUI thread, and preserve offline determinism. It needs, and must first
   design, (a) a scheduler-thread snapshot + command queue and (b) a consumer-owned
   ring flush for stop/restart. Run `make test`, `make asan` and `make tsan` after.
2. A real audio backend: follow `AUDIO-BACKENDS.md` (miniaudio recommended). The ALSA
   backend must still be run on a real machine (`make ODY_WITH_ALSA=1`) before any
   claim about it.
