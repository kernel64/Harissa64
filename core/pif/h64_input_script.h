// Harissa64 V2 - scripted controller input, shared by h64test (--input) and
// the Xbox front end (input= in harissa64v2.ini), so that the same scene
// can be played on the host and in Xenia.
//
// Syntax: comma-separated events BUTTON@first[-last] (VI frames), buttons
// A B Z START L R DU DD DL DR CU CD CL CR; stick X=n@.. and Y=n@..
#ifndef H64_INPUT_SCRIPT_H
#define H64_INPUT_SCRIPT_H

#include "../common/h64_types.h"

struct H64System;

struct H64InputEvent
{
    u32 first, last;
    u16 buttons;
    int axis;    // 0 none, 1 X, 2 Y
    int value;
};

struct H64InputScript
{
    H64InputEvent events[256];
    int count;
};

// Returns 0, or -1 on a syntax error (logged).
int h64_input_script_parse(H64InputScript *s, const char *text);
// Sets controller 1 from the events active at the current VI frame.
void h64_input_script_apply(const H64InputScript *s, H64System *sys);

#endif
