#pragma once
#include <stddef.h>
#include <stdint.h>
#include "protocol.h"

// What survives a reboot, in NVS: the state from the phone and the refreshes
// logged on the watch that the phone has not confirmed yet.

static constexpr int MAX_PENDING = 8;

struct Saved {
  char name[40] = "Madre";
  int16_t tz = 0;                       // minutes east of UTC
  uint8_t nRefreshes = 0;
  int64_t refreshes[MADRE_MAX_REFRESHES];  // from the phone, newest first
  uint8_t nPending = 0;
  int64_t pending[MAX_PENDING];         // logged here, not yet confirmed
};

void storeLoad(Saved &out);
void storeSave(const Saved &s);
// The companion's id the phone asked for (madre/comp); empty = Madre. Loading never fails.
void storeLoadCompanion(char *out, size_t size);
void storeSaveCompanion(const char *id);
