// Harissa64 - Xbox 360 achievements (see xb_ach.h).
#include "xb_ach.h"

#include <xtl.h>
#include <xbox.h>
#include <string.h>

#include "../../core/common/h64_log.h"
#include "../../core/common/h64_types.h"

// XUserWriteAchievements keeps its arguments until the write is done: each
// request has its own slot, never reused in a session (a list this short is
// asked for once each at most).
struct AchSlot
{
    XUSER_ACHIEVEMENT ach;
    XOVERLAPPED ov;
};
static AchSlot s_slots[XB_ACH_COUNT + 1];
static u8 s_asked[XB_ACH_COUNT + 1];
static CRITICAL_SECTION s_lock;
static int s_ready, s_enabled;

void XbAchInit(int enabled)
{
    if (!s_ready)
    {
        InitializeCriticalSection(&s_lock);
        s_ready = 1;
    }
    s_enabled = enabled;
}

void XbAchUnlock(int id)
{
    DWORD user, r;
    AchSlot *s;
    if (!s_ready || !s_enabled || id < 1 || id > XB_ACH_COUNT) return;
    EnterCriticalSection(&s_lock);
    if (s_asked[id])
    {
        LeaveCriticalSection(&s_lock);
        return;
    }
    for (user = 0; user < 4; user++)
        if (XUserGetSigninState(user) != eXUserSigninState_NotSignedIn) break;
    if (user == 4)
    {
        // No profile: asked again next time (the player may sign in later).
        LeaveCriticalSection(&s_lock);
        H64_INFO("[ach] achievement %d: no profile signed in", id);
        return;
    }
    s_asked[id] = 1;
    s = &s_slots[id];
    memset(s, 0, sizeof(*s));
    s->ach.dwUserIndex = user;
    s->ach.dwAchievementId = (DWORD)id;
    r = XUserWriteAchievements(1, &s->ach, &s->ov);
    LeaveCriticalSection(&s_lock);
    H64_INFO("[ach] achievement %d for profile %lu: %s (%lu)", id, user, r == ERROR_IO_PENDING || r == ERROR_SUCCESS ? "asked" : "refused", r);
}
