#!/usr/bin/env python3
"""Independent test oracles for the ODYSSEY integration tests.

IMPORTANT: this file is TEST CODE ONLY.  It computes what the AGC program is
EXPECTED to emit so the tests can compare it with what the AGC actually wrote.
It is never used to produce audio.  The host (C code) contains no oscillator.

usage:
  oracle.py phase  CSV STEP_OCT          check PHASE-* output against a model
  oracle.py timing CSV                   print inter-sample cycle statistics
  oracle.py seq    CSV w1,w2,...         check an exact word sequence (octal)
  oracle.py bus    CSV A|B               check a BUS-RECV/BUS-DUP* receiver report
"""
import csv
import statistics
import sys


def rows(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def step14(u, s):
    """Model of the PHASE-* program for a non-negative phase and step.

    The AGC adds in a 16-bit accumulator.  For phase 037700 + step 0100 the sum
    is 040000: the accumulator OVERFLOWS (sign bit S1 = 0 but S2 = 1).  TS then
    stores the overflow-CORRECTED word: sign from S1 (positive), magnitude = the
    low 14 bits.  So the stored phase is (phase + step) mod 2**14: a clean
    unipolar sawtooth 0 .. +16383 that wraps to +0.  (An earlier version of this
    oracle wrongly modelled a mod-32767 wrap into the negative half; the AGC
    output showed that was the oracle's mistake, not the program's.)
    """
    return (u + s) & 0o37777


def check_phase(path, step):
    r = rows(path)
    if not r:
        print("phase: no samples"); return 1
    u = 0
    bad = 0
    for i, row in enumerate(r):
        u = step14(u, step)
        got = int(row["word_octal"], 8)
        if got != u:
            bad += 1
            if bad <= 5:
                print(f"phase: sample {i}: AGC {got:05o} != model {u:05o}")
    seqs = [int(x["seq"]) for x in r]
    if seqs != list(range(len(r))):
        print("phase: sequence numbers are not 0..N-1"); bad += 1
    print(f"phase: {len(r)} samples checked, {bad} mismatches")
    return 1 if bad else 0


def cycles(path):
    return [int(x["cycle"]) for x in rows(path)]


def timing(path):
    c = cycles(path)
    d = [b - a for a, b in zip(c, c[1:])]
    if not d:
        print("timing: need >= 2 samples"); return 1
    if any(x <= 0 for x in d):
        print("timing: NON-MONOTONIC cycle stamps"); return 1
    agc_hz = (1024000 + 6) // 12
    mean = statistics.fmean(d)
    print(f"timing: n={len(c)} mean={mean:.3f} min={min(d)} max={max(d)} "
          f"stdev={statistics.pstdev(d):.3f} cycles/sample; "
          f"mean rate={agc_hz / mean:.1f} Hz (min-period rate "
          f"{agc_hz / min(d):.1f} Hz, max-period rate {agc_hz / max(d):.1f} Hz)")
    return 0


# Independent transcription of the two message sets in flight/tests/BUS-DATA.agc.
# Each message is (TYPE, SEQ, DATA0, DATA1), octal values.
BUS_SETS = {
    "A": [(0o1, 0o77773, 0o1, 0o77777), (0o2, 0o77774, 0o12345, 0o0),
          (0o3, 0o77775, 0o37777, 0o40000), (0o4, 0o77776, 0o65432, 0o777),
          (0o5, 0o77777, 0o1234, 0o54321), (0o6, 0o0, 0o77777, 0o1),
          (0o7, 0o1, 0o20000, 0o17777), (0o10, 0o2, 0o10, 0o20),
          (0o1, 0o3, 0o31415, 0o27172), (0o2, 0o4, 0o77776, 0o77775),
          (0o3, 0o5, 0o100, 0o7070), (0o4, 0o6, 0o40001, 0o36000)],
    "B": [(0o10, 0o100, 0o11111, 0o22222), (0o7, 0o101, 0o33333, 0o44444),
          (0o6, 0o102, 0o55555, 0o66666), (0o5, 0o103, 0o77777, 0o0),
          (0o4, 0o104, 0o1010, 0o10101), (0o3, 0o105, 0o2020, 0o20202),
          (0o2, 0o106, 0o3030, 0o30303), (0o1, 0o107, 0o4040, 0o40404),
          (0o10, 0o110, 0o5050, 0o50505), (0o7, 0o111, 0o6060, 0o60606),
          (0o6, 0o112, 0o7070, 0o70707), (0o5, 0o113, 0o1, 0o2)],
}


def check_bus(path, which):
    """The receiver ROM reports 5 words per message on the audio channel:
    TYPE SEQ DATA0 DATA1 ERR, where ERR is the AGC's own compare result."""
    got = [int(x["word_octal"], 8) for x in rows(path)]
    want = []
    for m in BUS_SETS[which]:
        want.extend(m)
        want.append(0)          # ERR == 0: the AGC itself found a match
    bad = 0
    if len(got) != len(want):
        print(f"bus: {len(got)} words reported, expected {len(want)}"); bad += 1
    for i, (g, w) in enumerate(zip(got, want)):
        if g != w:
            bad += 1
            if bad <= 5:
                print(f"bus: word {i}: AGC {g:05o} != expected {w:05o}")
    print(f"bus: set {which}: {len(got)} words checked, {bad} problems")
    return 1 if bad else 0


def check_seq(path, words):
    got = [x["word_octal"] for x in rows(path)]
    want = [f"{int(w, 8):05o}" for w in words.split(",")]
    if got != want:
        print(f"seq: MISMATCH\n  got : {got}\n  want: {want}")
        return 1
    print(f"seq: {len(got)} words match exactly")
    return 0


if __name__ == "__main__":
    mode = sys.argv[1]
    if mode == "phase":
        sys.exit(check_phase(sys.argv[2], int(sys.argv[3], 8)))
    if mode == "timing":
        sys.exit(timing(sys.argv[2]))
    if mode == "seq":
        sys.exit(check_seq(sys.argv[2], sys.argv[3]))
    if mode == "bus":
        sys.exit(check_bus(sys.argv[2], sys.argv[3]))
    print(__doc__); sys.exit(2)
