/* SPDX-License-Identifier: GPL-2.0-or-later */

#include <stdio.h>
#include <stdlib.h>

#include "odyssey_engine.h"

static int
Fail(const char *Message, OdysseyComputer *Computer, const char *RomPath)
{
  fprintf(stderr, "single-instance host check failed: %s\n", Message);
  odyssey_computer_destroy(Computer);
  if (RomPath != NULL)
    remove(RomPath);
  return 1;
}

int
main(void)
{
  static const char RomPath[] = "odyssey-single-host-test.bin";
  static const unsigned char RomWord[] = { 0x00, 0x00 };
  OdysseyComputer Computer;
  OdysseyChannelEvent Event;
  FILE *RomFile;

  RomFile = fopen(RomPath, "wb");
  if (RomFile == NULL)
    return Fail("could not create ROM fixture", NULL, NULL);
  if (fwrite(RomWord, 1, sizeof(RomWord), RomFile) != sizeof(RomWord))
    {
      fclose(RomFile);
      return Fail("could not write ROM fixture", NULL, RomPath);
    }
  if (fclose(RomFile) != 0)
    return Fail("could not close ROM fixture", NULL, RomPath);

  if (odyssey_computer_init(&Computer, RomPath) != 0)
    return Fail("host could not initialize the embedded AGC", &Computer,
                RomPath);
  if (Computer.Engine.agc_clientdata != &Computer)
    return Fail("AGC instance does not point back to its host context",
                &Computer, RomPath);
  if (odyssey_computer_step(&Computer, 16) != 0 ||
      Computer.Engine.CycleCounter != 16)
    return Fail("bounded host stepping did not advance the AGC", &Computer,
                RomPath);

  /* Drain engine-originated writes so the following event is unambiguous. */
  while (odyssey_computer_pop_output(&Computer, &Event))
    ;

  /* Channel 0300 falls through the engine's generic I/O handling. */
  CpuWriteIO(&Computer.Engine, 0300, 012345);
  if (!odyssey_computer_pop_output(&Computer, &Event))
    return Fail("host buffer did not receive the channel write", &Computer,
                RomPath);
  if (Event.Source != &Computer || Event.Channel != 0300 ||
      Event.Value != 012345 || Event.CycleCounter != 16)
    return Fail("captured event lost its source, value, channel, or cycle",
                &Computer, RomPath);
  if (odyssey_computer_pop_output(&Computer, &Event))
    return Fail("host buffer returned an unexpected extra event", &Computer,
                RomPath);

  odyssey_computer_destroy(&Computer);
  if (remove(RomPath) != 0)
    return Fail("could not remove ROM fixture", NULL, NULL);

  puts("single-instance host passed: 16 bounded engine steps, channel 0300 "
       "captured with source and cycle, host and fixture cleaned up");
  return 0;
}
