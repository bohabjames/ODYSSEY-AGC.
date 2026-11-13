# ODYSSEY virtual spacecraft bus (milestone 6)

**Everything here is an ODYSSEY extension. None of it is an Apollo interface.**
The authoritative text is the header comment in `odyssey_bus.h`; this file adds
the rationale, the engine facts it rests on, and the measured behaviour.

## What it is

An in-process, single-threaded host component that moves COMPLETE four-word
messages between two host-side directional queues (CMC->LGC, LGC->CMC). No
sockets, no TCP, no shared AGC memory: an AGC can only affect the other through
a committed message, and only sees the other's data through its own input
channels.

## Channels (same numbers in both computers; the host routes by `agc_t *`)

| Octal | Direction | Meaning |
|---|---|---|
| 0310 | AGC->host | TX TYPE (reserved value 0 = invalid) |
| 0311 | AGC->host | TX SEQ (15 bits, wraps 077777 -> 0) |
| 0312 | AGC->host | TX DATA0 |
| 0313 | AGC->host | TX DATA1 |
| 0314 | AGC->host | TX COMMIT (value must equal the staged SEQ) |
| 0320 | host->AGC | RX STATUS: bit 1 (01) VALID, bit 2 (02) OVERFLOW (sticky until reset) |
| 0321-0324 | host->AGC | RX TYPE, SEQ, DATA0, DATA1 (all 0 while VALID = 0). **Read-only for the AGC** |
| 0325 | AGC->host | RX ACK: write the SEQ of the message being acknowledged |

Payload words are opaque 15-bit values in M6. No musical meaning is defined yet.

## Transmit rules (judged in this order on every write to 0314)

1. not all four fields written since the last commit attempt -> `incomplete`
2. COMMIT != staged SEQ -> `invalid_commits`
3. TYPE == 0 -> `invalid_commits`
4. sequence vs the sender's last accepted SEQ, using **unsigned modular**
   arithmetic `delta = (SEQ - last) & 077777`:
   `0` -> `duplicates`; `1..037777` -> accepted (skipped numbers added to
   `seq_gaps`); `>= 040000` -> `out_of_order`
5. accepted: appended to the sender's queue (256 messages). If full: newest is
   dropped, `queue_overflows` and `dropped` increment, the receiver's sticky
   OVERFLOW bit is set, and the sender is never blocked. A message dropped for
   overflow still counts as the sender's last SEQ, so the receiver sees a gap.

Every commit attempt, good or bad, clears the staging area, so stale fields can
never complete a later message. Rewriting a field before commit is allowed
(last write wins). The first message after power-on/reset may use any SEQ.

## Receive / mailbox

Each computer has a one-message mailbox backed by the other computer's queue. A
message in the mailbox is **stable until acknowledged** with a matching SEQ on
0325. A wrong ack (or an ack with nothing valid) only increments
`invalid_acks`. A matching ack releases the mailbox; the next message appears at
the **next scheduler boundary**, not immediately.

## Delivery timing (contract with `odyssey_scheduler.c`)

`odyssey_bus_deliver()` runs once per round, after both computers have finished
their slices and their audio has been drained. So a message committed in round
N is first readable in round N+1, in both directions, and never mid-instruction.
At most one message per computer per round is delivered.

Consequences, both measured (see below):
* Maximum rate is one message per round per direction: about `85333 / slice`
  messages per AGC-second (slice 1000 -> about 85/s).
* Slice size changes WHEN a message arrives, never WHAT arrives or in what
  order. Results for one slice size are bit-for-bit repeatable.

## How the receiving side reaches the AGC (verified in the engine source)

* `READ` -> `ReadIO()` returns `State->InputChannel[Address]` (L/Q excepted).
  So the bus uses ordinary input-channel values; no counters or interrupts.
* `agc_engine()` calls `ChannelInput(State)` at the top of **every** engine
  call. `odyssey_io.c` implements it to republish channels 0320-0324 from the
  host mailbox into **that State's own** `InputChannel[]`. Nothing is ever
  written into the other computer's `agc_t`.
* **Hazard found and handled:** `WriteIO()` stores every CPU write into
  `InputChannel[Address]`, so an AGC `WRITE 0321` would overwrite the mailbox
  the AGC reads. Republishing on every engine call restores it before the next
  instruction; such writes are counted in `rx_write_violations`.

## Reset / resynchronisation of computer X

`odyssey_computer_reset(X)` (and the scheduler wrapper) calls
`odyssey_bus_reset_computer()`: X's staging, last-SEQ memory, mailbox and
sticky overflow are cleared, and both queues touching X are emptied (every
discarded message counted in `dropped`). The **other** computer's mailbox is
left alone and stays stable until acked. Counters are cumulative and survive
resets; `resets` counts them.

## Diagnostics (per computer; printed by `--bus` as `cmc_bus_*` / `lgc_bus_*`)

`tx_messages rx_messages acked invalid_commits incomplete duplicates
out_of_order seq_gaps invalid_acks queue_overflows dropped rx_write_violations
resets ack_timeouts max_ack_wait_rounds pending overflow_sticky`.
`--ack-timeout N` flags a mailbox message unacknowledged for more than N rounds
(reported once per message; off by default).

## Evidence (all from `make test`)

* `tests/test_bus.c`: protocol/queue/mailbox/reset/ownership/hook checks with no
  AGC; mutation-tested (9 deliberately injected bugs, each caught).
* `flight/tests/BUS-*.agc`: real AGC programs. The receiver uses actual `READ`
  instructions on 0320-0324, compares every field with its own table inside the
  AGC (ERR word), acknowledges with `WRITE 0325`; the host oracle independently
  checks the reported words. Set A deliberately crosses the 077777 -> 0 wrap.
* CMC->LGC, LGC->CMC and simultaneous duplex all pass; slice sizes 1, 7, 1000,
  4096 give identical receiver words; repeat runs are byte-identical.
* Controls: no bus attached -> receiver gets nothing; two receivers/no sender ->
  nothing, no watchdog restarts; two senders/no acks -> exactly one message in
  each mailbox, 12 pending, ack timeout reported, nothing lost.

Measured (this sandbox; measurements, not guarantees): 12 messages delivered in
about 12 rounds, i.e. last report at cycle 12103 for slice 1000, 49251 for slice
4096, 1465 for slice 1 (limited by the AGC's own speed).

## Limits and open points

* The bus is owned by the scheduler thread and is not thread-safe: no atomics, and
  neither an audio callback nor a UI thread may call it. (M8 added cross-thread
  audio, but only through the separate SPSC PCM rings in `odyssey_rt`; the bus is
  untouched by it.)
* No interrupt delivery; receivers must poll 0320.
* Message TYPE meanings and payloads for the synth ROMs are defined in `SYNTH-PROTOCOL.md` (M7).
* `--bus-log FILE` (M7, host-side record only) writes `C,round,sender,type,seq,d0,d1` per accepted commit, `D,round,receiver,seq` per delivery, `K,round,receiver,seq` per ack (octal fields).
* 0301 is a record-only status word (M7).
* The one-message-per-round limit is deliberate (determinism); a deeper mailbox
  would raise throughput but must be specified first.
