#!/usr/bin/env python3
"""Independent model of the milestone-7 synth programs (CMC-SYNTH / LGC-SYNTH).

The model is written from the algorithm descriptions in docs/SYNTH-PROTOCOL.md
and the program headers, NOT by running the AGC code.  Inputs are only what the
host recorded:

  * the audio CSV of the computer under test   (seq,cycle,word_octal,...)
  * that computer's channel-0301 status log     (seq,cycle,word_octal)
  * the bus log                                  (event,round,who,a,b,c,d)

Cause and effect are aligned by cycle stamps: a status word with an EARLIER
cycle than a sample was written before that sample was computed (see the
per-sample order contract in the program headers).  The k-th status word of a
computer corresponds to the k-th message committed by the OTHER computer
(mailbox delivery is strictly in order, every message is acknowledged).

usage:
  oracle_synth.py cmc AUDIO.csv STATUS.csv BUSLOG [--note-offset N]
                                      check the CMC stream + its sent messages
  oracle_synth.py lgc AUDIO.csv STATUS.csv BUSLOG       check the LGC stream + its sent messages
  oracle_synth.py cmc-nobus AUDIO.csv                   CMC run WITHOUT a bus: pure note table, no bend
  oracle_synth.py lgc-nobus AUDIO.csv                   LGC run WITHOUT a bus: all samples must be 0

Exit status 0 only if the AGC's output matches the model exactly.
"""
import csv
import sys

# ---- independent transcription of the programs' constants -----------------
NOTEINC = [1800, 2025, 2250, 2700, 3000, 3600, 2700, 2250]
VELTAB = [16000, 12000, 14000, 10000, 15000, 16383, 11000, 13000]
STEPLEN = 200
NSTEPS = 8
HOLDTAB = [8, 7, 6, 4, 3, 2, 4, 6]
LCG_A, LCG_C, LCG_SEED = 1229, 1013, 1
DECAY = 100
MODPER = 400
BSCALE, BOFF = 400, 200
M14 = 0x3FFF
TYPE_SYNC, TYPE_PITCH, TYPE_MOD, TYPE_TRIG = 1, 2, 3, 4
IGNORED = 0o40


def read_samples(path):
    with open(path) as f:
        return [(int(r["cycle"]), int(r["word_octal"], 8)) for r in csv.DictReader(f)]


def read_status(path):
    with open(path) as f:
        return [(int(r["cycle"]), int(r["word_octal"], 8)) for r in csv.DictReader(f)]


def read_commits(path):
    """Accepted commits per sender, in order: {0: [(type,seq,d0,d1)], 1: [...]}"""
    out = {0: [], 1: []}
    with open(path) as f:
        for r in csv.DictReader(f):
            if r["event"] == "C":
                out[int(r["who"])].append(tuple(int(r[k], 8) for k in ("a", "b", "c", "d")))
    return out


def signed15(w):
    """15-bit one's complement -> int (both zeros -> 0)."""
    return -((~w) & 0x7FFF) if w & 0x4000 else w


def enc15(v):
    """int -> 15-bit one's complement as the AGC produces it.  A sum x + (-x) is
    -0 (077777) in one's complement arithmetic, so a computed zero that came
    from adding a negative offset is encoded as 077777."""
    if v == 0:
        return 0x7FFF
    return v if v > 0 else (0x7FFF - (-v))


class Report:
    def __init__(self, name):
        self.name, self.bad = name, 0

    def fail(self, msg):
        self.bad += 1
        if self.bad <= 8:
            print(f"{self.name}: MISMATCH {msg}")


def check_cmc(audio, status, commits, note_offset=0):
    r = Report("cmc")
    samples = read_samples(audio)
    events = read_status(status)
    from_lgc = commits[1]                     # messages the CMC receives, in order
    sent = commits[0]                         # messages the CMC sent

    if len(events) > len(from_lgc):
        r.fail(f"{len(events)} status words but only {len(from_lgc)} messages from the LGC")
    phase, bend, wave, ei = 0, 0, 0, 0
    applied = 0
    for n, (cyc, word) in enumerate(samples):
        while ei < len(events) and events[ei][0] < cyc:
            mtype, _seq, d0, d1 = from_lgc[ei]
            want_status = mtype if mtype == TYPE_MOD else (mtype | IGNORED)
            if events[ei][1] != want_status:
                r.fail(f"status word {ei}: {events[ei][1]:05o} != {want_status:05o}")
            if mtype == TYPE_MOD:
                bend, wave = signed15(d0), d1 & 1
                applied += 1
            ei += 1
        step = (n // STEPLEN) % NSTEPS
        phase = (phase + NOTEINC[step] + bend) & M14
        want = phase if wave == 0 else (M14 if phase >= 0x2000 else 0)
        if word != want:
            r.fail(f"sample {n} (cycle {cyc}): AGC {word:05o} != model {want:05o} "
                   f"(step {step} bend {bend} wave {wave})")

    # messages the CMC sent: one SYNC/PITCH/TRIGGER triple per started step
    N = len(samples)
    started = (N - 1) // STEPLEN + 1
    if not (3 * started <= len(sent) <= 3 * started + 3):
        r.fail(f"{len(sent)} messages sent, expected {3 * started}..{3 * started + 3}")
    for i, (t, seq, d0, d1) in enumerate(sent):
        k = (i // 3) % NSTEPS
        want = [(TYPE_SYNC, i, k, 0), (TYPE_PITCH, i, NOTEINC[k], (k + note_offset) & 7),
                (TYPE_TRIG, i, VELTAB[k], k)][i % 3]   # TRIGGER DATA1 = step
        if (t, seq, d0, d1) != want:
            r.fail(f"sent message {i}: {(t, seq, d0, d1)} != {want}")
    print(f"cmc: {N} samples checked exactly, {applied} MODULATION messages applied, "
          f"{len(sent)} messages sent, {r.bad} problems")
    return r.bad


def check_lgc(audio, status, commits):
    r = Report("lgc")
    samples = read_samples(audio)
    events = read_status(status)
    from_cmc = commits[0]
    sent = commits[1]

    if len(events) > len(from_cmc):
        r.fail(f"{len(events)} status words but only {len(from_cmc)} messages from the CMC")
    nz, held, hcnt, hset, env, scnt = LCG_SEED, 0, 0, 1, 0, 0
    ei = 0
    expect_mod = []
    nonzero = 0
    for n, (cyc, word) in enumerate(samples):
        while ei < len(events) and events[ei][0] < cyc:
            mtype, _seq, d0, d1 = from_cmc[ei]
            applied = mtype in (TYPE_SYNC, TYPE_PITCH, TYPE_TRIG)
            want_status = mtype if applied else (mtype | IGNORED)
            if events[ei][1] != want_status:
                r.fail(f"status word {ei}: {events[ei][1]:05o} != {want_status:05o}")
            if mtype == TYPE_PITCH:
                hset = HOLDTAB[d1 & 7]
            elif mtype == TYPE_TRIG:
                env = d0
            ei += 1
        if hcnt > 0:
            hcnt -= 1
        else:
            nz = (nz * LCG_A + LCG_C) & M14
            held = nz
            hcnt = hset - 1
        want = (env * held) >> 14
        if word != want:
            r.fail(f"sample {n} (cycle {cyc}): AGC {word:05o} != model {want:05o} "
                   f"(env {env} held {held} hset {hset})")
        nonzero += word != 0
        if env > 0:
            env = max(env - DECAY, 0)
        scnt += 1
        if scnt == MODPER:
            scnt = 0
            bend = ((held * BSCALE) >> 14) - BOFF
            expect_mod.append((TYPE_MOD, len(expect_mod), enc15(bend), nz & 1))

    # allow the run to end while the last send is still in progress
    if not (len(expect_mod) - 1 <= len(sent) <= len(expect_mod) + 1):
        r.fail(f"{len(sent)} MODULATION messages sent, model expects {len(expect_mod)}")
    for i, (got, want) in enumerate(zip(sent, expect_mod)):
        if got != want:
            r.fail(f"sent MODULATION {i}: {got} != {want}")
    print(f"lgc: {len(samples)} samples checked exactly ({nonzero} non-zero), "
          f"{len(events)} messages applied, {len(sent)} MODULATION messages sent, {r.bad} problems")
    return r.bad


def check_cmc_nobus(audio):
    """Without a bus no message can ever arrive: the CMC must play the plain note
    table (bend 0, sawtooth) and nothing else."""
    r = Report("cmc-nobus")
    samples = read_samples(audio)
    phase = 0
    for n, (cyc, word) in enumerate(samples):
        phase = (phase + NOTEINC[(n // STEPLEN) % NSTEPS]) & M14
        if word != phase:
            r.fail(f"sample {n}: AGC {word:05o} != plain-table model {phase:05o}")
    print(f"cmc-nobus: {len(samples)} samples checked exactly, {r.bad} problems")
    return r.bad


def check_lgc_nobus(audio):
    """Without a bus the LGC never receives a TRIGGER, so its envelope stays 0 and
    every sample it computes is 0: the host adds no sound of its own."""
    samples = read_samples(audio)
    nz = sum(1 for _c, w in samples if w != 0)
    print(f"lgc-nobus: {len(samples)} samples, {nz} non-zero")
    return 1 if (nz or not samples) else 0


if __name__ == "__main__":
    a = sys.argv[1:]
    if len(a) in (4, 6) and a[0] == "cmc":
        off = int(a[5]) if len(a) == 6 and a[4] == "--note-offset" else 0
        sys.exit(1 if check_cmc(a[1], a[2], read_commits(a[3]), off) else 0)
    if len(a) == 4 and a[0] == "lgc":
        sys.exit(1 if check_lgc(a[1], a[2], read_commits(a[3])) else 0)
    if len(a) == 2 and a[0] == "cmc-nobus":
        sys.exit(1 if check_cmc_nobus(a[1]) else 0)
    if len(a) == 2 and a[0] == "lgc-nobus":
        sys.exit(check_lgc_nobus(a[1]))
    print(__doc__)
    sys.exit(2)
