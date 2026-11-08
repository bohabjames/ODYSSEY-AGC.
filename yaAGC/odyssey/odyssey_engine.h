/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef ODYSSEY_ENGINE_H
#define ODYSSEY_ENGINE_H

#include <stddef.h>
#include <stdint.h>

#include "agc_engine.h"

#define ODYSSEY_OUTPUT_QUEUE_CAPACITY 64

typedef struct OdysseyComputer OdysseyComputer;

typedef struct
{
  OdysseyComputer *Source;
  int Channel;
  int Value;
  uint64_t CycleCounter;
} OdysseyChannelEvent;

struct OdysseyComputer
{
  agc_t Engine;
  OdysseyChannelEvent OutputQueue[ODYSSEY_OUTPUT_QUEUE_CAPACITY];
  size_t OutputHead;
  size_t OutputCount;
  uint64_t OutputOverflows;
  int Initialized;
};

/* Return the upstream agc_engine_init result, or -1 for invalid arguments. */
int odyssey_computer_init(OdysseyComputer *Computer, const char *RomImage);

/* Execute a bounded number of upstream engine calls; never waits for I/O. */
int odyssey_computer_step(OdysseyComputer *Computer, unsigned int Budget);

/* Pop the oldest captured channel event; return 1 when an event was read. */
int odyssey_computer_pop_output(OdysseyComputer *Computer,
                                OdysseyChannelEvent *Event);

/* Clear an initialized instance. The ODYSSEY host owns no engine heap data. */
void odyssey_computer_destroy(OdysseyComputer *Computer);

#endif
