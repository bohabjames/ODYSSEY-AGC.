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
