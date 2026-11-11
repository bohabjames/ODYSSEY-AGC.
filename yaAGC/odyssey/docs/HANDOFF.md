# ODYSSEY-AGC handoff (Claude -> Codex)

Branch history: M1-M6 merged to `master` (PRs #1, #2). M7 is on `claude/odyssey-m7`.

## State

Milestones 1-7 of AGENTS.md are done, each with test evidence. **Milestone 8
(real-time audio) is NOT started.**

| M | Status | Evidence |
|---|---|---|
| 1 engine runs embedded | done | upstream demo clean, also under ASan/UBSan |
| 2 AGC writes audio words (SEQTEST) | done | exactly 8 expected words, 0 alarm restarts |
| 3 AGC-side synthesis (PHASE-*) | done | AGC output equals independent Python model (about 145k samples); SILENT yields 0 samples |
| 4 state isolation | done | differential vs frozen pristine engine bit-identical; interleaved/reset/destroy isolation; controls fail on the pristine engine |
| 5 dual execution | done | `test_sched` (70 checks) + CLI: dual output equals isolated runs at slices 1, 7, 1000, 4096, 299999; reset/destroy of one leaves the other untouched; mutation-tested |
| 7 dual synthesis | done | `SYNTH-CMC`/`SYNTH-LGC` match an independent exact model (base and ALT); bus-off controls; identical-prefix-then-diverge causality; closed-loop base vs ALT; slices 1/7/4096; deterministic; see `SYNTH-PROTOCOL.md` |
| 6 virtual bus | done | `test_bus` (about 422k checks incl. a 70000-message wrap lap) + BUS-* ROMs using real `READ`/`WRITE`; both directions and duplex; slice-invariant content; mutation-tested (9/9 caught) |

`make test` = 172 PASS lines, 0 FAIL, plus the isolation suite. `make asan` passes.
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
* `SYNTH-PROTOCOL.md` - M7 programs, message semantics, verification
* `STATE-ISOLATION-AUDIT.md`, `UPSTREAM-CHANGES.md`, `CHANNEL-VERIFICATION.md`

## Code map (yaAGC/odyssey)

`odyssey_engine.[ch]` computer lifecycle; `odyssey_io.c` engine hooks
(`ChannelOutput` routes audio/bus by `State->agc_clientdata`, `ChannelInput`
republishes the mailbox); `odyssey_audio.[ch]` sample queue + conversions;
`odyssey_capture.[ch]` drain/validate/CSV; `odyssey_scheduler.[ch]` round-robin
(round order is a documented contract); `odyssey_bus.[ch]` the bus;
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
* The audio ring and bus are single-thread only. M8 needs atomics (and TSan).
* Section 3 of `STATE-ISOLATION-AUDIT.md`: debugger, coverage, DEDA, telemetry
  and socket subsystems must stay off with more than one computer.
* `0301` is a record-only status word (M7); the host never acts on it.
* Message semantics are defined in `SYNTH-PROTOCOL.md` for the synth ROMs; the
  M6 BUS-* test ROMs still use opaque patterns. The receiver test ROMs report their results on
  the audio channel 0300 purely so the host can capture them.
* One message per round per direction is deliberate (determinism), so the
  scheduler slice bounds bus throughput (about 85333/slice messages per AGC-second).
* `tests/baseline/` is a frozen copy of pristine engine files; never edit it.

## Next steps (M8)

1. M8: real-time audio backend behind a small interface: atomics on the sample
   ring, underrun/overrun counters, independent gain/mute/solo, mixing.
