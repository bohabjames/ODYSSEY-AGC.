#!/bin/sh
# ODYSSEY-AGC integration tests (milestones 1-3 scope).
# Run via `make test YAYUL=/path/to/yaYUL` (which builds and assembles first).
# Exits non-zero if any check fails.
set -u

here=$(cd "$(dirname "$0")/.." && pwd)
build=${BUILD:-build}
cd "$here"
host="$build/odyssey-agc"
roms="$build/roms"
tmp="$build/testout"
mkdir -p "$tmp"

fails=0
pass() { echo "PASS  $1"; }
fail() { echo "FAIL  $1"; fails=$((fails + 1)); }
check() { # check "name" command...
  name=$1; shift
  if "$@" > "$tmp/last.log" 2>&1; then pass "$name"; else fail "$name"; sed 's/^/        /' "$tmp/last.log"; fi
}
stat() { sed -n "s/^$2=//p" "$1" | head -1; }
eq() { # eq "name" actual expected
  if [ "$2" = "$3" ]; then pass "$1 ($2)"; else fail "$1: got '$2', want '$3'"; fi
}
gt() { # gt "name" actual min
  if [ "$2" -gt "$3" ] 2>/dev/null; then pass "$1 ($2)"; else fail "$1: got '$2', want > $3"; fi
}

run() { # run ROM CYCLES CSV [extra args]  -> stats in $tmp/<csvbase>.stats
  rom=$1; cyc=$2; csv=$3; shift 3
  "$host" --rom "$roms/$rom.bin" --cycles "$cyc" --out "$csv" "$@" > "${csv%.csv}.stats"
}

echo "== unit tests =="
check "unit tests" "$build/test_unit" "$roms"
check "milestone 5 scheduler tests (test_sched)" "$build/test_sched" "$roms"

echo "== milestone 2: capture path (SEQTEST, 7 AGC-seconds) =="
run SEQTEST 600000 "$tmp/seq.csv"
s="$tmp/seq.stats"
eq "SEQTEST samples captured" "$(stat $s samples)" 8
eq "SEQTEST samples emitted == captured" "$(stat $s samples_emitted)" 8
eq "SEQTEST no queue drops" "$(stat $s queue_dropped)" 0
eq "SEQTEST no sequence gaps" "$(stat $s seq_gaps)" 0
eq "SEQTEST no out-of-order" "$(stat $s out_of_order)" 0
eq "SEQTEST no timestamp regressions" "$(stat $s cycle_regressions)" 0
eq "SEQTEST audio-control write seen once" "$(stat $s audio_control_writes)" 1
eq "SEQTEST no AGC watchdog restarts in 7 s" "$(stat $s alarm_restarts)" 0
check "SEQTEST exact word sequence (incl. +0 and -0)" \
  python3 tests/oracle.py seq "$tmp/seq.csv" 00000,00001,37777,40000,77777,77776,12345,65432
check "SEQTEST PCM values: -0 -> 0, +-max -> +-32766" sh -c \
  "grep -q '^4,[0-9]*,77777,0,0\$' $tmp/seq.csv && grep -q '^2,[0-9]*,37777,16383,32766\$' $tmp/seq.csv && grep -q '^3,[0-9]*,40000,-16383,-32766\$' $tmp/seq.csv"

echo "== negative control: host emits nothing by itself (SILENT) =="
run SILENT 1000000 "$tmp/silent.csv"
s="$tmp/silent.stats"
eq "SILENT captured samples" "$(stat $s samples)" 0
eq "SILENT emitted samples" "$(stat $s samples_emitted)" 0
eq "SILENT no AGC watchdog restarts" "$(stat $s alarm_restarts)" 0

echo "== milestone 3: AGC-computed phase accumulator (oracle comparison) =="
for r in 500 1K 2K; do
  run "PHASE-$r" 2000000 "$tmp/phase$r.csv"
  s="$tmp/phase$r.stats"
  gt "PHASE-$r produced samples" "$(stat $s samples)" 1000
  eq "PHASE-$r no AGC watchdog restarts (23 AGC-seconds)" "$(stat $s alarm_restarts)" 0
  eq "PHASE-$r no sequence gaps" "$(stat $s seq_gaps)" 0
  eq "PHASE-$r no timestamp regressions" "$(stat $s cycle_regressions)" 0
  check "PHASE-$r every AGC word matches independent model" \
    python3 tests/oracle.py phase "$tmp/phase$r.csv" 00100
done

echo "== measured sample-generation rates (measurements, NOT guarantees) =="
for r in 500 1K 2K; do
  printf "PHASE-%-4s " "$r"
  python3 tests/oracle.py timing "$tmp/phase$r.csv" || fails=$((fails + 1))
done

echo "== determinism: output independent of slice size and of the run =="
base="$tmp/det_ref.csv"
run PHASE-1K 300000 "$base" --slice 1000
ref=$(md5sum < "$base")
for sl in 1 7 4096 299999 300000; do
  run PHASE-1K 300000 "$tmp/det_$sl.csv" --slice "$sl"
  eq "identical output with --slice $sl" "$(md5sum < "$tmp/det_$sl.csv")" "$ref"
done
run PHASE-1K 300000 "$tmp/det_again.csv" --slice 1000
eq "identical output on repeat run" "$(md5sum < "$tmp/det_again.csv")" "$ref"

echo "== overflow never blocks the AGC (one huge slice, 65,536-slot queue) =="
run PHASE-2K 20000000 "$tmp/ovf.csv" --slice 20000000
s="$tmp/ovf.stats"
eq "overflow run completed all cycles" "$(stat $s agc_cycles)" 20000000
gt "samples were dropped" "$(stat $s queue_dropped)" 0
eq "captured == queue capacity" "$(stat $s samples)" 65536
eq "emitted == captured + dropped" "$(stat $s samples_emitted)" \
   "$(( $(stat $s samples) + $(stat $s queue_dropped) ))"
eq "dropped counter == pushed shortfall" "$(stat $s queue_pushed)" 65536
eq "retained samples are the OLDEST, in order" \
   "$(python3 tests/oracle.py phase "$tmp/ovf.csv" 00100 | tail -1)" \
   "phase: 65536 samples checked, 0 mismatches"

echo "== milestone 5: dual execution via the CLI (CMC=PHASE-500, LGC=PHASE-1K) =="
dual() { # dual SLICE CYCLES TAG [cmc rom] [lgc rom]
  sl=$1; cy=$2; tag=$3; cr=${4:-PHASE-500}; lr=${5:-PHASE-1K}
  "$host" --cmc-rom "$roms/$cr.bin" --lgc-rom "$roms/$lr.bin" --cycles "$cy" --slice "$sl" \
    --out-cmc "$tmp/d_${tag}_cmc.csv" --out-lgc "$tmp/d_${tag}_lgc.csv" > "$tmp/d_${tag}.stats"
}
run PHASE-500 300000 "$tmp/iso_cmc.csv" --slice 1000
run PHASE-1K  300000 "$tmp/iso_lgc.csv" --slice 1000
iso_c=$(md5sum < "$tmp/iso_cmc.csv"); iso_l=$(md5sum < "$tmp/iso_lgc.csv")
[ "$iso_c" != "$iso_l" ] && pass "the two programs produce different streams (non-vacuous)" || fail "streams identical"
for sl in 1 7 1000 299999; do
  dual $sl 300000 "s$sl"
  eq "dual --slice $sl: CMC stream equals isolated CMC run" "$(md5sum < "$tmp/d_s${sl}_cmc.csv")" "$iso_c"
  eq "dual --slice $sl: LGC stream equals isolated LGC run" "$(md5sum < "$tmp/d_s${sl}_lgc.csv")" "$iso_l"
  s="$tmp/d_s$sl.stats"
  eq "dual --slice $sl: CMC cycles" "$(stat $s cmc_agc_cycles)" 300000
  eq "dual --slice $sl: LGC cycles" "$(stat $s lgc_agc_cycles)" 300000
  eq "dual --slice $sl: no watchdog restarts" "$(( $(stat $s cmc_alarm_restarts) + $(stat $s lgc_alarm_restarts) ))" 0
done
check "dual CMC stream matches independent model" python3 tests/oracle.py phase "$tmp/d_s7_cmc.csv" 00100
check "dual LGC stream matches independent model" python3 tests/oracle.py phase "$tmp/d_s7_lgc.csv" 00100
dual 1000 300000 again
eq "dual run repeats identically (CMC)" "$(md5sum < "$tmp/d_again_cmc.csv")" "$iso_c"
eq "dual run repeats identically (LGC)" "$(md5sum < "$tmp/d_again_lgc.csv")" "$iso_l"
dual 7 600000 seq SEQTEST PHASE-2K
eq "SEQTEST in CMC seat: exactly 8 samples" "$(stat "$tmp/d_seq.stats" cmc_samples)" 8
check "SEQTEST in CMC seat: exact words; unaffected by the busy LGC" \
  python3 tests/oracle.py seq "$tmp/d_seq_cmc.csv" 00000,00001,37777,40000,77777,77776,12345,65432
eq "dual CLI: argument error exits 2 (only one ROM)" \
  "$("$host" --cmc-rom "$roms/SEQTEST.bin" >/dev/null 2>&1; echo $?)" 2
eq "dual CLI: bad ROM path exits 4" \
  "$("$host" --cmc-rom /nonexistent.bin --lgc-rom "$roms/SEQTEST.bin" >/dev/null 2>&1; echo $?)" 4

echo
if [ "$fails" -eq 0 ]; then echo "ALL TESTS PASSED"; else echo "$fails CHECK(S) FAILED"; fi
exit "$fails"
