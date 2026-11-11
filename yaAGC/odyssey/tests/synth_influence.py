#!/usr/bin/env python3
"""Helpers that turn the milestone-7 'cross-computer influence' claims into
pass/fail checks.  All inputs are host-recorded logs.

  synth_influence.py prefix ON.csv ON_STATUS.csv OFF.csv
      ON = CMC audio with the bus, OFF = the same ROM without a bus.  The two
      must be IDENTICAL (same word and same cycle stamp) up to the first
      bus-message application, and different afterwards.
  synth_influence.py moddiff A.buslog B.buslog
      The LGC's MODULATION messages (who=1, type 3) must differ between the two runs.
  synth_influence.py streamdiff A.csv B.csv
      Two audio streams must differ in at least half of their common length.
"""
import csv
import sys


def samples(p):
    with open(p) as f:
        return [(int(r["cycle"]), int(r["word_octal"], 8)) for r in csv.DictReader(f)]


def status(p):
    with open(p) as f:
        return [(int(r["cycle"]), int(r["word_octal"], 8)) for r in csv.DictReader(f)]


def mods(p):
    out = []
    with open(p) as f:
        for r in csv.DictReader(f):
            if r["event"] == "C" and r["who"] == "1" and r["a"] == "3":
                out.append((r["c"], r["d"]))
    return out


def prefix(on_p, st_p, off_p):
    on, off, ev = samples(on_p), samples(off_p), status(st_p)
    n = min(len(on), len(off))
    first = next((i for i in range(n) if on[i] != off[i]), None)
    if not ev:
        print("prefix: the bus-on run never applied a message"); return 1
    if first is None:
        print("prefix: bus on and bus off are identical: no influence"); return 1
    differing = sum(on[i] != off[i] for i in range(first, n))
    ok = first > 0 and on[first][0] > ev[0][0] and differing >= 100
    print(f"prefix: {first} identical samples (word and cycle), first message applied at "
          f"cycle {ev[0][0]}, first difference at cycle {on[first][0]}, "
          f"{differing} of {n - first} later samples differ -> {'ok' if ok else 'BAD'}")
    return 0 if ok else 1


def moddiff(a_p, b_p):
    a, b = mods(a_p), mods(b_p)
    n = min(len(a), len(b))
    diff = sum(a[i] != b[i] for i in range(n))
    ok = n >= 10 and diff >= 0.8 * n
    print(f"moddiff: {diff} of {n} MODULATION messages differ -> {'ok' if ok else 'BAD'}")
    return 0 if ok else 1


def streamdiff(a_p, b_p):
    a, b = samples(a_p), samples(b_p)
    n = min(len(a), len(b))
    diff = sum(a[i][1] != b[i][1] for i in range(n))
    ok = n > 0 and diff * 2 >= n
    print(f"streamdiff: {diff} of {n} samples differ -> {'ok' if ok else 'BAD'}")
    return 0 if ok else 1


if __name__ == "__main__":
    a = sys.argv[1:]
    if len(a) == 4 and a[0] == "prefix":
        sys.exit(prefix(a[1], a[2], a[3]))
    if len(a) == 3 and a[0] == "moddiff":
        sys.exit(moddiff(a[1], a[2]))
    if len(a) == 3 and a[0] == "streamdiff":
        sys.exit(streamdiff(a[1], a[2]))
    print(__doc__)
    sys.exit(2)
