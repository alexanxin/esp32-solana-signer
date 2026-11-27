#include "blind_sign.h"

#include <Preferences.h>  // key-value storage in flash ("NVS")

namespace {
int gCached = -1;  // -1 = not read from flash yet, 0 = off, 1 = on
}

bool blindSigningAllowed() {
  if (gCached < 0) {
    Preferences prefs;
    prefs.begin("ui", true);  // same "folder" as the brightness; true = read only
    gCached = prefs.getBool("blind", false) ? 1 : 0;  // false if nothing saved: blocked
    prefs.end();
  }
  return gCached == 1;
}

void setBlindSigningAllowed(bool allowed) {
  gCached = allowed ? 1 : 0;
  Preferences prefs;
  prefs.begin("ui", false);
  prefs.putBool("blind", allowed);
  prefs.end();
}
