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

struct H64System;

enum { XB_RA_OFF = 0, XB_RA_OUT, XB_RA_BUSY, XB_RA_IN };

// `drive`: where config\ lives. `toast`: shows a message for `ms` (any thread).
void XbRaInit(const char *drive, int enabled, void (*toast)(const char *text, int ms));
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
};
int XbRaListBegin(void);
void XbRaListItem(int index, XbRaItem *item);
void XbRaListEnd(void);

// ratest=1: asks the server for the game id of SM64's hash without an account
// and logs the answer (checks the POST path). Blocks; returns the id or 0.
int XbRaSelfTest(void);

#endif
