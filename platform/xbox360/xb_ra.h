// Harissa64 - RetroAchievements (retroachievements.org) on the console:
// rcheevos' rc_client (third_party/rcheevos) with our HTTPS calls, the
// account, the running game's achievements and what they give on the Xbox
// side (xb_ach.h).
//
// Off unless the player turns it on (Settings > Achievements). The player
// types the account's name and password on the Xbox keyboard; only the token
// the server answers with is kept (<drive>:\config\ra.ini). Softcore only:
// save states stay allowed, and nothing is checked once a cheat was on.
#ifndef XB_RA_H
#define XB_RA_H

#include "xb_ui.h"

struct H64System;

enum { XB_RA_OFF = 0, XB_RA_OUT, XB_RA_BUSY, XB_RA_IN };

// `drive`: where config\ lives. `toast`: shows a message for `ms` (any
// thread), with the picture of the badge `badge` (XbRaBadge) when not NULL.
void XbRaInit(const char *drive, int enabled, void (*toast)(const char *text, int ms, const char *badge));
// Turns the whole thing on or off (also with a game running).
void XbRaSetEnabled(int on);
// XB_RA_*: off, no account, connecting, connected.
int XbRaState(void);
// The connected account's name ("" when none).
const char *XbRaUser(void);
// Settings > Account: connected, disconnects; otherwise opens the Xbox
// keyboard for the name, then the password, then connects.
void XbRaAccount(void);
// 1 while the Xbox keyboard is open (the menus then ignore the controller).
int XbRaBusy(void);
// Call often from the thread that runs the menus and the game: hands the
// server's answers to rc_client and follows the keyboard.
void XbRaPump(void);

// A game starts / ends (the system's viHook is taken while it runs).
void XbRaGameStart(H64System *sys);
void XbRaGameEnd(H64System *sys);
// After a reset or a loaded state: conditions in progress start again.
void XbRaReset(void);
// A save state was written to / loaded from statePath: the achievements'
// progress is kept beside it ("<statePath>.ra") and put back.
void XbRaStateSaved(const char *statePath);
void XbRaStateLoaded(const char *statePath);

// The running game's achievements for the menu: Begin returns their number
// (0: none, not connected or off) and holds the list until End.
struct XbRaItem
{
    const char *title, *description;
    int points, unlocked;
    char badge[24];   // its picture as it is now (earned or not), for XbRaBadge; "" when it has none
};
int XbRaListBegin(void);
void XbRaListItem(int index, XbRaItem *item);
void XbRaListEnd(void);

// Badges (the achievements' pictures) come from RetroAchievements in the
// background and are kept in <drive>:\art\badges\. XbRaListItem and an
// unlock ask for theirs. XbRaBadgesPump, on the device's thread and outside
// UiBegin/UiEnd, makes the textures of those that arrived and drops the ones
// no longer used; XbRaBadge gives a badge's texture, NULL while it is not there.
void XbRaBadgesPump(IDirect3DDevice9 *dev);
IDirect3DTexture9 *XbRaBadge(const char *name);

// ratoast=N: shows the unlock message of the game's first achievement, with
// its badge; nothing is unlocked.
void XbRaTestToast(void);

// ratest=1: asks the server for the game id of SM64's hash without an account
// and logs the answer (checks the POST path). Blocks; returns the id or 0.
int XbRaSelfTest(void);

#endif
