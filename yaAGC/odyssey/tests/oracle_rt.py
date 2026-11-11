#!/usr/bin/env python3
"""Independent model of the milestone-8 offline audio render.

Inputs are what the host recorded: the per-computer sample CSVs (pcm16 column)
and the WAV that --audio-wav produced.  The model re-derives every output frame:

  * TIMED mode (--stream 0, the default of odyssey-agc): frame f plays AGC time
    T = f * 85333 // rate - latency_cycles and holds the newest sample whose cycle
    stamp is <= T (0 before the first); the number of frames rendered must equal
    ceil((cycles + latency_cycles + 1) * rate / 85333);
  * FIXED mode (--stream N): zero-order hold: with step = (stream_rate << 32) // device_rate, after
    frame f the number of stream samples consumed is ((f + 1) * step) >> 32;
    the held value is pcm[consumed - 1] (0 before the first sample);
  * per-channel Q15 gain (round to nearest), clamp to +-32767,
  * audible = not muted and (no solo anywhere or this channel soloed),
  * sum audible channels, Q15 master gain (round to nearest), clamp +-32767,
  * mono mix duplicated to every output channel.

usage: oracle_rt.py CMC.csv LGC.csv OUT.wav [--rate N --stream N --channels N
         --gain-cmc G --gain-lgc G --master G --mute-cmc --mute-lgc --solo-cmc --solo-lgc]
Exit 0 only if the WAV equals the model exactly.
"""
import csv
import sys
import wave

UNITY = 32768


def q15(g):
    if not g > 0:
        return 0
    return min(int(g * UNITY + 0.5), 4 * UNITY)


def pcm(path):
    with open(path) as f:
        return [int(r["pcm16"]) for r in csv.DictReader(f)]


def pcm_timed(path):
    with open(path) as f:
        return [(int(r["cycle"]), int(r["pcm16"])) for r in csv.DictReader(f)]


def main(a):
    opt = dict(rate=48000, stream=0, latency=50, cycles=0, channels=2, gc=1.0, gl=1.0, master=1.0,
               mc=0, ml=0, sc=0, sl=0)
    pos, i = [], 0
    while i < len(a):
        k = a[i]
        if k == "--rate": opt["rate"] = int(a[i + 1]); i += 2
        elif k == "--stream": opt["stream"] = int(a[i + 1]); i += 2
        elif k == "--latency-ms": opt["latency"] = int(a[i + 1]); i += 2
        elif k == "--cycles": opt["cycles"] = int(a[i + 1]); i += 2
        elif k == "--channels": opt["channels"] = int(a[i + 1]); i += 2
        elif k == "--gain-cmc": opt["gc"] = float(a[i + 1]); i += 2
        elif k == "--gain-lgc": opt["gl"] = float(a[i + 1]); i += 2
        elif k == "--master": opt["master"] = float(a[i + 1]); i += 2
        elif k == "--mute-cmc": opt["mc"] = 1; i += 1
        elif k == "--mute-lgc": opt["ml"] = 1; i += 1
        elif k == "--solo-cmc": opt["sc"] = 1; i += 1
        elif k == "--solo-lgc": opt["sl"] = 1; i += 1
        else: pos.append(k); i += 1
    timed = opt["stream"] == 0
    if timed:
        cmc, lgc = pcm_timed(pos[0]), pcm_timed(pos[1])
    else:
        cmc, lgc = pcm(pos[0]), pcm(pos[1])
    wavp = pos[2]
    w = wave.open(wavp, "rb")
    ch, rate, width, n = w.getnchannels(), w.getframerate(), w.getsampwidth(), w.getnframes()
    raw = w.readframes(n)
    if (ch, rate, width) != (opt["channels"], opt["rate"], 2):
        print(f"oracle_rt: WAV header {ch}ch {rate}Hz {width*8}bit does not match options")
        return 1
    if len(raw) != n * ch * 2:
        print("oracle_rt: WAV data size does not match its header")
        return 1
    got = [int.from_bytes(raw[j:j + 2], "little", signed=True) for j in range(0, len(raw), 2)]

    MCT = 85333
    lat = opt["latency"] * MCT // 1000
    if timed:
        want_frames = -(-((opt["cycles"] + lat + 1) * opt["rate"]) // MCT)
        if n != want_frames:
            print(f"oracle_rt: WAV has {n} frames, timed model expects {want_frames}")
            return 1
    step = (opt["stream"] << 32) // opt["rate"] if not timed else 0
    gc, gl, ms = q15(opt["gc"]), q15(opt["gl"]), q15(opt["master"])
    anysolo = opt["sc"] or opt["sl"]
    au_c = (not opt["mc"]) and (not anysolo or opt["sc"])
    au_l = (not opt["ml"]) and (not anysolo or opt["sl"])
    bad = 0
    nz = 0
    pc = pl = 0
    c = l = 0
    for f in range(n):
        if timed:
            T = f * MCT // opt["rate"] - lat
            while pc < len(cmc) and cmc[pc][0] <= T:
                c = cmc[pc][1]; pc += 1
            while pl < len(lgc) and lgc[pl][0] <= T:
                l = lgc[pl][1]; pl += 1
            used = 1
        else:
            used = ((f + 1) * step) >> 32
        if not timed and used > min(len(cmc), len(lgc)):
            print(f"oracle_rt: frame {f} needs stream sample {used} but only "
                  f"{len(cmc)}/{len(lgc)} were produced (test configuration error)")
            return 1
        if not timed:
            c = cmc[used - 1] if used else 0
            l = lgc[used - 1] if used else 0
        vc = max(-32767, min(32767, (c * gc + 16384) // UNITY))
        vl = max(-32767, min(32767, (l * gl + 16384) // UNITY))
        mix = (vc if au_c else 0) + (vl if au_l else 0)
        out = max(-32767, min(32767, (mix * ms + 16384) // UNITY))
        nz += out != 0
        for k in range(ch):
            if got[f * ch + k] != out:
                bad += 1
                if bad <= 8:
                    print(f"oracle_rt: MISMATCH frame {f} ch {k}: wav {got[f*ch+k]} != model {out}")
    print(f"oracle_rt: {n} frames x {ch} ch checked exactly, {nz} non-zero frames, {bad} mismatches")
    return 1 if (bad or n == 0) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
