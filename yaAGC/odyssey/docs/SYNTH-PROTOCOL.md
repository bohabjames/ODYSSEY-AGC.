# Milestone 7: synthesis programs and message semantics

**ODYSSEY EXTENSION, not Apollo.** The programs in `flight/ComancheSynth/` and
`flight/LuminarySynth/` are new experimental AGC assembly (they only borrow
`flight/common/KEEPALIVE.agc`). No historical Comanche055/Luminary099 source is
edited. Every audio value is computed by AGC instructions; the host only
transports words (it adds no sound: see the bus-off control below).

## Programs

| ROM | Source | Role |
|---|---|---|
| `SYNTH-CMC` | `flight/ComancheSynth/CMC-SYNTH.agc` (+ `CMC-CORE.agc`) | tonal/rhythmic voice: 14-bit phase-accumulator oscillator, 8-step sequencer |
| `SYNTH-CMC-ALT` | `CMC-SYNTH-ALT.agc` (same core, `PNOTEOFS=3`) | test variant: tells the LGC a different note index |
| `SYNTH-LGC` | `flight/LuminarySynth/LGC-SYNTH.agc` | stochastic/modulation voice: LCG noise (a=1229,c=1013,seed 1), sample-and-hold, envelope, sends bend |

Both are assembled by `tools/build_roms.sh` together with `flight/common`.

## Channels (ODYSSEY experimental map)

`0300` audio sample; `0301` **status word** (defined in M7, record-only: the
host logs `seq,cycle,word` per write and never acts on it); `0310-0314` bus TX;
`0320-0324` bus RX (read-only); `0325` ack.

The status word is written by a program each time it consumes a bus message:
the message TYPE for an applied message, or `TYPE | 040` for a type that
computer ignores.

## Messages (TYPE 5-8 reserved, unused)

| Type | Name | Direction | DATA0 | DATA1 |
|---|---|---|---|---|
| 1 | SYNC | CMC->LGC | step number | 0 |
| 2 | PITCH | CMC->LGC | phase increment of the note | `(step + PNOTEOFS) & 7` |
| 4 | TRIGGER | CMC->LGC | velocity | step |
| 3 | MODULATION | LGC->CMC | signed pitch bend -200..199 (one's complement; computed 0 is 077777) | waveform select bit (0 saw, 1 square) |

Effects: the LGC uses PITCH DATA1 to select its sample-and-hold rate
(`HOLDTAB = 8,7,6,4,3,2,4,6`) and TRIGGER DATA0 to set its envelope, which
decays by 100 per sample. Every 400 LGC samples it sends
`bend = ((held*400)>>14) - 200` and the low bit of its noise state. The CMC adds
the bend to its phase increment and switches waveform. Hence CMC -> LGC ->
CMC closed loop.

## Per-sample order contract

Status writes with an earlier cycle stamp than a sample were consumed before
that sample was computed. Applying a message also costs AGC time, so cycle
stamps after the first message shift relative to a bus-off run; compare by
sample index, not absolute cycle, after that point.

## Verification (`tests/run_tests.sh`, milestone 7 section)

* `tests/oracle_synth.py`: independent exact model written from this document;
  checks every sample and every sent message (base, ALT with `--note-offset 3`,
  and slices 1/7/4096). It was mutation-tested (injected bugs are caught).
* Bus-off controls: CMC plays the plain note table; LGC output is all zero.
* `tests/synth_influence.py`: CMC with bus equals bus-off for the identical
  prefix (word and cycle) up to the first applied MODULATION, then diverges;
  base vs ALT produce different MODULATION messages and different CMC audio.
* Determinism: two identical runs give byte-identical audio, status and bus logs.
* Measured (1,000,000 cycles, slice 1000): 11201 CMC / 11553 LGC samples,
  28 MODULATION messages applied, first divergence at sample 407, 0 alarm
  restarts, all bus reject counters 0.

## Limits

* Bus delivery is once per scheduler round, so the slice size changes message
  latency in cycles (and thus the audio after the first message). Exactness is
  verified at each slice against the model, not equality across slices.
* Sample rates are measured per run, not guaranteed. Real-time playback of these
  programs is M8 (`AUDIO-ENGINE.md`); it makes no rate or latency guarantee.
* The programs run inside the scheduler, which is single-threaded: both AGCs, the bus
  and sample capture share one thread. In M8 the only other thread is the audio
  callback, which never touches the computers or the bus; it reads PCM from SPSC
  rings. (M7 itself was single-threaded; the playback path is M8.)
