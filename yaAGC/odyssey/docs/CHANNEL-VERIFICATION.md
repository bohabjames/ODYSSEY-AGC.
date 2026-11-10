# Experimental channel map: verification

AGENTS.md: "Document any channel conflict and revise the allocation rather than
silently overriding existing behavior." This file records what was checked
against the actual engine source and what was observed.

**Everything at octal `0300`-`0325` is an ODYSSEY extension. None of it is an
Apollo channel definition.**

## Map in use (defined in `odyssey_config.h`)

| Channel (octal) | Name | Status |
|---|---|---|
| `0300` | `ODY_CH_AUDIO_SAMPLE` | **implemented**: one 15-bit audio word per write |
| `0301` | `ODY_CH_AUDIO_CONTROL` | **implemented minimally**: latched value + write counter, no behaviour yet |
| `0310`-`0314` | bus transmit (type, seq, data0, data1, commit) | **implemented** (milestone 6), see `BUS-PROTOCOL.md` |
| `0320`-`0324` | bus receive (status, type, seq, data0, data1), host -> AGC input channels | **implemented** (milestone 6); read-only for the AGC |
| `0325` | bus receive acknowledge, AGC -> host | **implemented** (milestone 6) |

## Checks performed against the source

1. **Range.** `NUM_CHANNELS` is 512 (`agc_engine.h`), valid octal `0000`-`0777`.
   `0300`-`0325` are in range. `WriteIO()` ignores addresses above `0777`.
2. **Instruction reach.** The AGC `WRITE`/`READ` instructions take a 9-bit channel
   address, `000`-`777` octal (per the VirtualAGC/ibiblio Assembly Language
   Manual). `WRITE 0300` therefore assembles and executes.
3. **Callback routing.** Every CPU write goes `CpuWriteIO()` -> `WriteIO()` ->
   `ChannelOutput(State, channel, value & 077777)`, with the originating `agc_t *`.
   That is what lets ODYSSEY route by instance.
4. **Engine special cases.** The engine gives meaning to low channels (e.g. 01/02
   L/Q, 010, 011, 013, 014 (gyro/IMU), 015/016, 033, 034/035, 077, the fake DSKY
   channel `0163`, and fake-IMU outputs `0171`, `0172`, `0177`). A grep of
   `agc_engine.c` and `agc_engine_init.c` found **no reference to any channel
   numbered `0200` or above**, so nothing overlaps `0300`-`0325`. **No conflict
   found.** (An earlier draft listed 03, 07, 012, 030-032 as special-cased; that
   was not confirmed in the source and has been removed.)
5. **Observed behaviour.** `SEQTEST` writes channel `0300` eight times and `0301`
   once; the host captured exactly those, in order, and the engine's own
   background traffic (see below) never landed on them.

## Other channels the host sees (diagnostic, not a conflict)

* `0163` (value `00200`): the engine's fake DSKY channel, lighting RESTART on
  cycle 0. Appears once at start-up in every run. It is below `0300`.
* `077`: alarm bits, written by the engine on every watchdog restart. ODYSSEY uses
  non-zero values to count restarts (`alarm_restarts`). See
  `AGC-PROGRAMMING-RULES.md`.
* `0171`, `0172`, `0177`: fake IMU CDU/gyro output channels. Only appear if a program
  drives the IMU. Not used by ODYSSEY synthesis.
* All other unhandled writes are counted in `other_channel_writes`; the latest
  channel/value are in `last_other_channel` / `last_other_value`.

## Verified in milestone 6

* `READ` of an ordinary channel returns `State->InputChannel[Address]`
  (`ReadIO()`, `agc_engine.c`), so the bus publishes 0320-0324 there.
* The engine calls `ChannelInput(State)` at the top of every `agc_engine()` call;
  that is where each computer's own mailbox is copied into its own
  `InputChannel[]`. Evidence: `tests/test_bus.c` (hook tests) and the BUS-* ROMs
  reading the fields with real `READ` instructions (all fields correct, both
  directions, including the 077777 -> 0 sequence wrap).
* **Hazard:** `WriteIO()` also stores every AGC write into `InputChannel[]`, so
  an AGC `WRITE` to 0320-0324 would clobber the mailbox. Republishing every
  engine call restores it before the next instruction; the write is counted as
  `rx_write_violations`. Tested by corrupting the array and stepping once.
* No conflict: the engine references no channel numbered 0200 or above.

## Still open

* The intended semantics of `0301` (mute/gain/flags) are undefined; nothing
  depends on it.
