#include "store.h"
#include <Preferences.h>

static constexpr uint8_t STORE_VERSION = 1;

void storeLoad(Saved &out) {
  Preferences p;
  if (!p.begin("madre", true)) return;
  if (p.getUChar("v", 0) == STORE_VERSION && p.getBytesLength("state") == sizeof(Saved)) p.getBytes("state", &out, sizeof(Saved));
  p.end();
  out.name[sizeof(out.name) - 1] = 0;
  if (out.nRefreshes > MADRE_MAX_REFRESHES) out.nRefreshes = 0;
  if (out.nPending > MAX_PENDING) out.nPending = 0;
}

void storeLoadCompanion(char *out, size_t size) {
  out[0] = 0;
  Preferences p;
  if (!p.begin("madre", true)) return;
  p.getString("comp", out, size);
  p.end();
  out[size - 1] = 0;
}

void storeSaveCompanion(const char *id) {
  Preferences p;
  if (!p.begin("madre", false)) return;
  p.putString("comp", id);
  p.end();
}

void storeSave(const Saved &s) {
  Preferences p;
  if (!p.begin("madre", false)) return;
  p.putUChar("v", STORE_VERSION);
  p.putBytes("state", &s, sizeof(Saved));
  p.end();
}
