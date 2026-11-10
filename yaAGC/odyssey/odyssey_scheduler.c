/*
 * ODYSSEY-AGC -- Filename: odyssey_scheduler.c
 * See odyssey_scheduler.h for the round contract.
 */
#include <string.h>
#include "odyssey_scheduler.h"

int
odyssey_scheduler_init (odyssey_scheduler_t *s, odyssey_computer_t *cmc,
                        odyssey_computer_t *lgc, uint32_t slice)
{
  if (s == NULL || cmc == NULL || lgc == NULL || slice == 0 || cmc == lgc)
    return -1;
  if (!cmc->initialized || !lgc->initialized)
    return -2;
  if (cmc->id != ODY_COMPUTER_CMC || lgc->id != ODY_COMPUTER_LGC)
    return -3;
  memset (s, 0, sizeof *s);
  s->computer[ODY_COMPUTER_CMC] = cmc;
  s->computer[ODY_COMPUTER_LGC] = lgc;
  s->slice = slice;
  ody_capture_init (&s->capture[ODY_COMPUTER_CMC], NULL);
  ody_capture_init (&s->capture[ODY_COMPUTER_LGC], NULL);
  return 0;
}

void
odyssey_scheduler_set_csv (odyssey_scheduler_t *s, int id, FILE *csv)
{
  if (s != NULL && id >= 0 && id < ODY_MAX_COMPUTERS)
    s->capture[id].csv = csv;
}

void
odyssey_scheduler_attach_bus (odyssey_scheduler_t *s, odyssey_bus_t *bus)
{
  int id;
  if (s == NULL)
    return;
  s->bus = bus;
  for (id = 0; id < ODY_MAX_COMPUTERS; id++)
    s->computer[id]->bus = bus;
}

void
odyssey_scheduler_round (odyssey_scheduler_t *s, uint32_t budget)
{
  int id;

  /* 1-2: each computer runs its bounded slice; nothing crosses between them */
  for (id = 0; id < ODY_MAX_COMPUTERS; id++)
    (void) odyssey_computer_step (s->computer[id], budget);
  /* 3-4: drain audio */
  for (id = 0; id < ODY_MAX_COMPUTERS; id++)
    ody_capture_drain (&s->capture[id], s->computer[id]);
  /* 5: bus delivery boundary */
  if (s->bus != NULL)
    odyssey_bus_deliver (s->bus);
  /* 6: bookkeeping */
  s->rounds++;
}

uint64_t
odyssey_scheduler_run (odyssey_scheduler_t *s, uint64_t total)
{
  uint64_t given = 0, rounds = 0;

  while (given < total)
    {
      uint64_t left = total - given;
      uint32_t n = (left < s->slice) ? (uint32_t) left : s->slice;
      odyssey_scheduler_round (s, n);
      given += n;
      rounds++;
    }
  return rounds;
}

int
odyssey_scheduler_reset_computer (odyssey_scheduler_t *s, int id)
{
  int rc;

  if (s == NULL || id < 0 || id >= ODY_MAX_COMPUTERS)
    return -1;
  rc = odyssey_computer_reset (s->computer[id]);
  if (rc != 0)
    return rc;
  ody_capture_reset (&s->capture[id]);
  return 0;
}
