# Writing AGC test/synth programs for ODYSSEY: rules learned the hard way

Everything below was observed in this repository's engine or assembler, not
assumed. References: the VirtualAGC Assembly Language Manual
(https://www.ibiblio.org/apollo/assembly_language_manual.html) and
https://github.com/NeilFraser/AGC-code (small example programs).

## 1. Watchdogs: the engine restarts a program that does not feed them

On a trip the engine does a GOJAM restart and calls `ChannelOutput(077, bits)`;
the host counts it as `alarm_restarts`. A non-zero count means the AGC program
is broken, not that the host is. Bits: 00004 TC Trap, 00010 Rupt Lock, 00020
Night Watchman.

* **TC Trap (checked every 5 ms, about 430 machine cycles).** In each window the
  program must execute at least one TC/TCF **and** at least one other
  instruction. A loop of only TCF, or with no TC/TCF, trips it. A poll loop such
  as `READ / MASK / CCS / TCF` is fine. (The engine also treats CS, DXCH, TS and
  XCH as causing TC-line transients; do not rely on that.)
* **Night Watchman:** any access to address 067 (NEWJOB) within about 1.28 s.
* **Rupt Lock / NoRupt (about 160 ms):** the program must take interrupts and
  return from them with RESUME, and must not sit inside an interrupt.
* Use `flight/common/KEEPALIVE.agc` at the top of every program. It supplies the
  vector table at 04000, a T3RUPT roughly every 40 ms that touches NEWJOB, and
  the register symbols. Define a `MAIN` label and start it with `RELINT`.

## 2. Arithmetic facts that bit us

* Words are 15-bit one's complement: +0 = 000000, -0 = 077777, max +037777.
  A + (-A) gives **-0**, so test equality with `CCS A` and treat both zeros as
  equal (see `CHKZ` in `BUS-CORE.agc`).
* `TS` of an overflowed A stores the sign-corrected low 14 bits, sets A to +-1
  and skips the next instruction. That makes a phase accumulator wrap mod 2^14
  (a unipolar sawtooth), not through the negative half.
* `CCS K`: A = |K| - 1 (K unchanged), then continue at `>0` I+1, `+0` I+2,
  `<0` I+3, `-0` I+4. `CCS A` is the usual loop/zero test.
* Timings: CA, AD, CCS, TS, READ, WRITE, RESUME take 2 machine cycles; TCF and
  EXTEND take 1. One machine cycle = 12 / 1.024 MHz, 85333 per second.

## 3. Registers: fixed addresses

A = 0, **L = 1, Q = 2**, EB = 3, FB = 4, Z = 5. `RETURN` is `TC Q`, so the
assembler needs `Q EQUALS 2`. **Defining `Q EQUALS 1` silently makes RETURN jump
through L to address 0** and the program runs its own registers as code until
the TC Trap fires (this cost a debugging session; see git history of
`BUS-CORE.agc`). If a subroutine "works alone but restarts", check Q first.

## 4. yaYUL gotchas

* `yaYUL --force` writes a `.bin` even after errors. Judge success by the
  "Fatal errors" summary; `tools/build_roms.sh` does this and was negative-tested.
* Numbers in `OCT`/address fields are octal: a stray 8 or 9 is a fatal error
  ("Not an octal number"), e.g. `27182` is invalid.
* Labels are at most 8 characters.
* `EQUALS` needs its right-hand symbol already defined, so put the data include
  first, then `TXTAB EQUALS ATAB`, then the code include (see `BUS-DUP*.agc`).
  Labels used by instructions may be defined later.
* `$FILE.agc` includes a file; `tools/build_roms.sh` copies all of
  `flight/tests` and `flight/common` into one scratch directory per ROM.
* Indexed table access: `INDEX IDX` then `CA TAB +1` loads `TAB+1+c(IDX)`.
  Tables placed with `SETLOC 6000` live in fixed-fixed memory (reachable
  without bank switching).
* Erasable scratch starts at `0100` in these programs; yaYUL does not check
  that it is free.

## 5. ODYSSEY I/O from AGC code (extension channels, not Apollo)

* Audio: `WRITE 0300` emits one 15-bit sample word; the host only transports it.
* Bus: see `BUS-PROTOCOL.md`. Poll `READ 0320` bit 1, read 0321-0324, `WRITE 0325`
  with the SEQ. Never `WRITE` 0320-0324.
* `EXTEND` must precede `READ`/`WRITE`.
