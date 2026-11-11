# Milestone 7 plan: dual synthesis with measurable cross-influence

Status: PLAN ONLY. No M7 code written yet. Gate (AGENTS.md): two AGC programs
that each calculate their own samples, exchange musical messages over the bus,
and a bus message **measurably changes** the receiver's output. The host must
not generate or alter any waveform.

## Decisions to confirm first (cheap, but they shape everything)

1. **Message set for M7.** Implement only what is needed to prove influence;
   defer the rest.
   * Slice A (required): `PITCH` (2), `TRIGGER` (4).
   * Slice B (if budget allows): `SYNC` (1), `MODULATION` (3).
   * Not in M7: `PARAMETER`, `HEARTBEAT`, `RESET`, `ACK` message types.
2. **Payload layouts** (15-bit words; unsigned unless stated):
   * `PITCH`: DATA0 = phase increment per sample (1..037777), DATA1 = voice (0..3).
   * `TRIGGER`: DATA0 = voice, DATA1 = velocity (0..037777; 0 = note off).
   * `MODULATION`: DATA0 = destination id, DATA1 = depth, one's-complement signed.
   * `SYNC`: DATA0 = beat counter (wraps at 037777), DATA1 = cycles per beat / 64.
   Ranges and invalid-value behaviour get written into `BUS-MESSAGES.md` with
   test vectors before any AGC code.
3. **Sample pacing.** Reuse the verified PHASE pattern (delay loop + 14-bit TS
   phase accumulator). Polling the bus costs cycles, so poll once per N samples
   and measure the resulting rate; do not promise a rate.

## Work breakdown (each step ends in a passing test)

| # | Step | Evidence |
|---|---|---|
| 1 | `BUS-MESSAGES.md`: types, payloads, ranges, units | doc + Python encode/decode vectors |
| 2 | `CMC-SYNTH.agc`: one voice, phase accumulator, square + stepped triangle. Defaults to a fixed pitch with no bus traffic | Python oracle model matches AGC output sample-for-sample; no restarts |
| 3 | CMC polls the bus, acks, applies `PITCH`/`TRIGGER` | scripted sender ROM sends PITCH at a known point; oracle predicts exact step change |
| 4 | `LGC-SYNTH.agc`: 15-bit LFSR noise + pulse-pattern mutation, own samples | oracle LFSR model matches |
| 5 | LGC transmits `PITCH`/`TRIGGER` to the CMC from its sequence | CMC output differs from step 2 in exactly the oracle-predicted way |
| 6 | Influence proof (below) + slice-invariance + determinism | tests in `run_tests.sh` |
| 7 | (budget permitting) `SYNC`, `MODULATION` | same pattern |
| 8 | Docs: HANDOFF, BUS-MESSAGES, measured rates | |

## Proof that the bus changes the output (the M7 gate)

* **A/B run:** CMC-SYNTH alongside a silent LGC, bus attached, vs the same with
  the LGC transmitting. Output streams must be identical **before** the first
  message is acted on and differ **after** it, with the difference matching the
  oracle (new phase step) rather than just "different".
* **Control 1:** bus not attached -> CMC output equals the no-message run.
* **Control 2:** a message with an invalid TYPE or failed commit -> no change.
* **Control 3:** same messages, slice sizes 1, 7, 1000, 4096 -> identical sample
  *values* after the message; timing of the change is compared in "samples after
  receipt", not absolute cycles, because delivery time depends on slice size
  (documented in `BUS-PROTOCOL.md`).
* **Host-does-not-synthesize check:** an AGC that never writes 0300 still gives
  zero samples (existing SILENT control) with the bus active.

## Risks and how to handle them

* **Cycle budget:** polling + ack + sample math may drop the rate well below
  the PHASE ROM rates. Measure first (step 2/3); lower the sample rate or poll
  less often. Report measured numbers only.
* **Watchdogs:** every loop needs TC/TCF + a non-TC instruction; keep KEEPALIVE.
  Read `AGC-PROGRAMMING-RULES.md` first (Q register, CCS zeros, TS overflow).
* **Fixed/erasable space:** tables and state are small; keep tables in
  fixed-fixed (`SETLOC 6000`) as in `BUS-DATA.agc`.
* **Throughput:** one message per round per direction. Musical events must be
  sparse (a few per AGC-second at slice 1000) or the slice must be reduced; this
  is a design constraint, not a bug.
* **Do not touch** `tests/baseline/`, Comanche055/Luminary099 source, or the
  upstream engine files. If M7 seems to need an engine change, stop and write the
  evidence first.
* **"Comanche/Luminary-derived":** AGENTS.md calls these programs
  Comanche/Luminary-*derived*, but the milestone gate only requires distinct
  AGC-side programs. Plan: standalone experimental programs (as the PHASE/BUS
  ROMs are) and keep the historical trees unmodified. **Needs your OK.**

## Budget-aware order

Minimum useful stopping point is after step 5 (CMC voice + LGC noise source +
one PITCH message audibly changing the CMC, proven by the oracle). Steps 7-8 can
be left for Codex; `HANDOFF.md` should be updated at each stop.

## Out of scope (M8)

Audio device, mixing, gain/mute/solo, atomics/thread safety, real-time pacing.
