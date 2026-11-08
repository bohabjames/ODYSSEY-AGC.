<!-- SPDX-License-Identifier: GPL-2.0-or-later -->

# ODYSSEY AGC integration

ODYSSEY is kept separate from the historical mission software and the normal
VirtualAGC executable. This directory starts with a test-only gate for the
embedded AGC engine; it is not the synthesizer host yet.

## Milestone 1: embedded baseline

`../EmbeddedDemo.c` is designed for embedded firmware. Its `CoreRope` array is
supplied by the firmware linker, and its main loop runs forever, so the demo
cannot provide a bounded test result or an orderly host cleanup check. The
`odyssey-embedded-baseline` CTest target therefore drives the same upstream AGC
engine through its public initialization and execution functions with a tiny
generated ROM fixture. It checks ROM loading, bounded instruction execution,
instance-aware input/routine/output callback calls, and cleanup of its fixture.

This is a documented validation adaptation: the firmware-only infinite loop is
not run directly. No engine behavior, historical Apollo source, or upstream
licensing text is changed. The only upstream build-file change is the
`yaAGC/CMakeLists.txt` hook that adds this separate target.

Run it with:

```text
cmake --workflow default
ctest --test-dir build -R odyssey-embedded-baseline --output-on-failure
```
