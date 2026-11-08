<!-- SPDX-License-Identifier: GPL-2.0-or-later -->

# ODYSSEY AGC integration

ODYSSEY is kept separate from the historical mission software and the normal
VirtualAGC executable. This directory starts with a test-only gate for the
embedded AGC engine; it is not the synthesizer host yet.

## Milestone 1: embedded baseline

`odyssey-embedded-demo` compiles and runs the upstream `EmbeddedDemo.c` entry
point with a test `CoreRope` image. Its infinite loop is bounded by a test
wrapper after 16 engine calls. The test checks embedded ROM transcoding,
execution, input/routine/output callback routing, and explicit state cleanup.

`odyssey-embedded-baseline` also checks the public `agc_engine_init` file-ROM
path with a small generated fixture, including bounded execution, callbacks,
and fixture cleanup. Neither test changes the engine behavior, historical
Apollo source, or upstream licensing text. The only upstream build-file change
is the `yaAGC/CMakeLists.txt` hook that adds these separate ODYSSEY targets.

Run it with:

```text
cmake --workflow default
ctest --test-dir build -R odyssey-embedded-baseline --output-on-failure
ctest --test-dir build -R odyssey-embedded-demo --output-on-failure
```

## Milestone 2: single-instance host

`odyssey_engine.c` owns one `agc_t` and its bounded channel-event queue. The
engine's existing `agc_clientdata` field routes each `ChannelOutput` callback
to its owning host object. The initial queue holds 64 events and drops the
newest write when full; the producer never waits. Events retain source, channel,
value, and the AGC cycle counter. The host's `UnblockSocket` implementation is
a no-op so embedding the engine does not alter process stdin settings.

The test exercises channel `0300` (`CpuWriteIO`'s generic path in the checked-in
engine), checks the routed event and its cycle timestamp, and verifies fixture
and instance cleanup. It does not yet generate samples from an AGC synthesis
program; that is the next milestone.

```text
ctest --test-dir build -R odyssey-single-host --output-on-failure
```
