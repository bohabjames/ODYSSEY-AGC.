/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Milestone 1 integration check for the upstream VirtualAGC engine.
 *
 * This new file is part of ODYSSEY and does not modify historical mission
 * software or replace the upstream engine.
 */

#include <stdio.h>
#include <string.h>

#include "agc_engine.h"

static agc_t *ExpectedState;
static unsigned InputCalls;
static unsigned RoutineCalls;
static unsigned OutputCalls;
static int CallbackMismatch;
static int LastOutputChannel;
static int LastOutputValue;

static void
CheckState(agc_t *State)
{
  if (State != ExpectedState)
    CallbackMismatch = 1;
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

void
BacktraceAdd(agc_t *State, int Cause)
{
  (void) State;
  (void) Cause;
}

static int
Fail(const char *Message, const char *RomPath)
{
  fprintf(stderr, "embedded baseline failed: %s\n", Message);
  if (RomPath != NULL)
    remove(RomPath);
  return 1;
}

int
main(void)
{
  static const char RomPath[] = "odyssey-embedded-baseline.bin";
  static const unsigned char RomWord[] = { 0x00, 0x00 };
  agc_t State;
  FILE *RomFile;
  int i;
  int Result;

  memset(&State, 0, sizeof(State));
  ExpectedState = &State;

  RomFile = fopen(RomPath, "wb");
  if (RomFile == NULL)
    return Fail("could not create ROM fixture", NULL);
  if (fwrite(RomWord, 1, sizeof(RomWord), RomFile) != sizeof(RomWord))
    {
      fclose(RomFile);
      return Fail("could not write ROM fixture", RomPath);
    }
  if (fclose(RomFile) != 0)
    return Fail("could not close ROM fixture", RomPath);

  Result = agc_engine_init(&State, RomPath, NULL, 0);
  if (Result != 0)
    return Fail("agc_engine_init did not load the ROM fixture", RomPath);
  if (State.Fixed[2][0] != 0 || State.Erasable[0][RegZ] != 04000)
    return Fail("ROM word or initial program counter was not loaded", RomPath);

  for (i = 0; i < 16; i++)
    agc_engine(&State);

  if (State.CycleCounter != 16)
    return Fail("engine did not advance through 16 bounded cycles", RomPath);
  if (InputCalls != 16 || RoutineCalls == 0)
    return Fail("engine input/routine callbacks were not invoked", RomPath);

  CpuWriteIO(&State, 0163, 012345);
  if (OutputCalls == 0 || LastOutputChannel != 0163 ||
      LastOutputValue != 012345)
    return Fail("engine output callback did not receive the channel write", RomPath);
  if (CallbackMismatch)
    return Fail("a callback received the wrong AGC state pointer", RomPath);

  if (remove(RomPath) != 0)
    return Fail("could not remove ROM fixture during cleanup", NULL);

  printf("embedded baseline passed: ROM loaded, %u cycles, %u input calls, "
         "%u routine calls, %u output calls, fixture removed\n",
         (unsigned) State.CycleCounter, InputCalls, RoutineCalls, OutputCalls);
  return 0;
}
