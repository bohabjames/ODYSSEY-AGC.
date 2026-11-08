/* SPDX-License-Identifier: GPL-2.0-or-later */

#include <string.h>

#include "odyssey_engine.h"

int
odyssey_computer_init(OdysseyComputer *Computer, const char *RomImage)
{
  int Result;

  if (Computer == NULL || RomImage == NULL)
    return -1;

  memset(Computer, 0, sizeof(*Computer));
  Computer->Engine.agc_clientdata = Computer;
  Result = agc_engine_init(&Computer->Engine, RomImage, NULL, 0);
  if (Result != 0)
    {
      memset(Computer, 0, sizeof(*Computer));
      return Result;
    }

  /* agc_engine_init calls UnblockSocket; the ODYSSEY host keeps it a no-op. */
  Computer->Engine.agc_clientdata = Computer;
  Computer->Initialized = 1;
  return 0;
}

int
odyssey_computer_step(OdysseyComputer *Computer, unsigned int Budget)
{
  unsigned int i;

  if (Computer == NULL || !Computer->Initialized)
    return -1;

  for (i = 0; i < Budget; i++)
    (void) agc_engine(&Computer->Engine);
  return 0;
}

int
odyssey_computer_pop_output(OdysseyComputer *Computer,
                            OdysseyChannelEvent *Event)
{
  if (Computer == NULL || Event == NULL || !Computer->Initialized ||
      Computer->OutputCount == 0)
    return 0;

  *Event = Computer->OutputQueue[Computer->OutputHead];
  Computer->OutputHead = (Computer->OutputHead + 1) %
                         ODYSSEY_OUTPUT_QUEUE_CAPACITY;
  Computer->OutputCount--;
  return 1;
}

void
odyssey_computer_destroy(OdysseyComputer *Computer)
{
  if (Computer != NULL)
    memset(Computer, 0, sizeof(*Computer));
}

/* Route channel writes through the agc_t host context, without global IDs. */
void
ChannelOutput(agc_t *State, int Channel, int Value)
{
  OdysseyComputer *Computer;
  size_t Slot;

  if (State == NULL || State->agc_clientdata == NULL)
    return;
  Computer = (OdysseyComputer *) State->agc_clientdata;
  if (&Computer->Engine != State || !Computer->Initialized)
    return;

  if (Computer->OutputCount == ODYSSEY_OUTPUT_QUEUE_CAPACITY)
    {
      /* Bounded, non-blocking policy: discard the newest event. */
      Computer->OutputOverflows++;
      return;
    }

  Slot = (Computer->OutputHead + Computer->OutputCount) %
         ODYSSEY_OUTPUT_QUEUE_CAPACITY;
  Computer->OutputQueue[Slot].Source = Computer;
  Computer->OutputQueue[Slot].Channel = Channel;
  Computer->OutputQueue[Slot].Value = Value;
  Computer->OutputQueue[Slot].CycleCounter = State->CycleCounter;
  Computer->OutputCount++;
}

/* Embedded ODYSSEY has no TCP peripherals or process-level stdin setup. */
void
UnblockSocket(int SocketNum)
{
  (void) SocketNum;
}

int
ChannelInput(agc_t *State)
{
  (void) State;
  return 0;
}

void
ChannelRoutine(agc_t *State)
{
  (void) State;
}

void
ShiftToDeda(agc_t *State, int Data)
{
  (void) State;
  (void) Data;
}

void
RequestRadarData(agc_t *State)
{
  (void) State;
}

void
BacktraceAdd(agc_t *State, int Cause)
{
  (void) State;
  (void) Cause;
}
