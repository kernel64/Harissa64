// Harissa64 - Xbox 360 achievements (the title's own list, in the XEX: see
// tools/gen_achievements.py). XbAchUnlock gives one to the signed-in profile:
// the console shows its notification and keeps it in the profile, where the
// guide and Aurora list it. Without a signed-in profile nothing happens.
#ifndef XB_ACH_H
#define XB_ACH_H

#include "xb_ach_ids.h"

// `enabled` 0 (Xenia, checks): XbAchUnlock does nothing.
void XbAchInit(int enabled);
// Gives achievement `id` (XB_ACH_*). Safe from any thread; returns at once.
// One already given in this session is not asked for again.
void XbAchUnlock(int id);

#endif
