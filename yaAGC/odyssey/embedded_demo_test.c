/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Run the upstream EmbeddedDemo entry point with an embedded CoreRope fixture.
 * The demo normally loops forever, so the wrapper exits after a fixed number
 * of calls to the upstream engine and lets the test inspect its state.
 */

#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include "agc_engine.h"

#define DEMO_CORE_SIZE (044 * 02000)
const unsigned char CoreRope[DEMO_CORE_SIZE][2] = {
  /* Initial bank 2 instructions are zero (NOP); offset 16 proves ROM copy. */
  { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 },
  { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 },
  { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 },
  { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 },
  { 0, 2 }
};

static jmp_buf DemoFinished;
static unsigned EngineCalls;
static unsigned InputCalls;
static unsigned RoutineCalls;
static unsigned OutputCalls;
static int CallbackMismatch;
static int LastOutputChannel;
static int LastOutputValue;

void odyssey_bounded_demo_engine(agc_t *State);

#define agc_engine odyssey_bounded_demo_engine
#define main odyssey_embedded_demo_main
#include "../EmbeddedDemo.c"
#undef main
#undef agc_engine

static void
CheckState(agc_t *Candidate)
{
  if (Candidate != &State)
    CallbackMismatch = 1;
}

void
odyssey_bounded_demo_engine(agc_t *State)
{
  (void) agc_engine(State);
  EngineCalls++;
  if (EngineCalls == 16)
    longjmp(DemoFinished, 1);
}

void
UnblockSocket(int SocketNum)
{
  (void) SocketNum;
}

void
ChannelOutput(agc_t *State, int Channel, int Value)
{
  CheckState(State);
  OutputCalls++;
  LastOutputChannel = Channel;
  LastOutputValue = Value;
}

int
ChannelInput(agc_t *State)
{
  CheckState(State);
  InputCalls++;
  return 0;
}

void
ChannelRoutine(agc_t *State)
{
  CheckState(State);
  RoutineCalls++;
}

void
ShiftToDeda(agc_t *State, int Data)
{
  CheckState(State);
  (void) Data;
}

void
RequestRadarData(agc_t *State)
{
  CheckState(State);
}

static int
Fail(const char *Message)
{
  fprintf(stderr, "embedded demo check failed: %s\n", Message);
  return 1;
}

int
main(void)
{
  if (setjmp(DemoFinished) == 0)
    (void) odyssey_embedded_demo_main();

  if (EngineCalls != 16 || State.CycleCounter != 16)
    return Fail("upstream demo did not execute 16 bounded engine steps");
  if (State.Fixed[2][16] != 1)
    return Fail("upstream demo did not transcode its embedded CoreRope");
  if (InputCalls != 16 || RoutineCalls == 0)
    return Fail("upstream demo callbacks were not invoked");

  CpuWriteIO(&State, 0163, 012345);
  if (OutputCalls == 0 || LastOutputChannel != 0163 ||
      LastOutputValue != 012345)
    return Fail("upstream engine output callback did not receive the write");
  if (CallbackMismatch)
    return Fail("a callback received the wrong AGC state pointer");

  /* EmbeddedDemo owns no heap or file resources; clear its state explicitly. */
  memset(&State, 0, sizeof(State));
  printf("upstream embedded demo passed: embedded ROM loaded, %u bounded "
         "cycles, callbacks routed, state cleared\n", EngineCalls);
  return 0;
}
