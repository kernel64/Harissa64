// Harissa64 V2 - front-end configuration (see xb_config.h).
#include "xb_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xb_audio.h"

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
}

void ConfigDefaults(Config *c)
{
    strcpy(c->mode, "play");
    strcpy(c->cpu, "dynarec");
    c->rom[0] = 0;
    c->lastRom[0] = 0;
    c->showFps = 0;
    c->menuAt = 0;
    c->autoStart = 0;
    c->autoIndex = 0;
    c->browserAt = 0;
    c->hle = 1;
    c->shotCount = 0;
    c->softRenderer = 0;
    c->xenosDebug = 0;
    c->pauseAt = 0;
    memset(&c->input, 0, sizeof(c->input));
    c->trace = c->traceStep = 0;
    c->xenia = 0;
    c->fpuFlags = 1;
    c->jitFpu = 1;
    c->regCache = 1;
    c->smooth = 1;
    c->asyncGfx = 1;
    c->asyncAudio = 1;
    c->audioCycles = 100000;
    c->gfxCycles = 400000;
    c->cpi = 1;
    c->exitAfter = 0;
    c->audioMs = XB_AUDIO_DEFAULT_MS;
    c->stateSlot = 1;
    c->loadState = 0;
    c->saveStateAt = 0;
}

int ConfigParseFile(Config *c, const char *path)
{
    FILE *f = fopen(path, "r");
    char line[1024];
    if (!f) return 0;
    while (fgets(line, sizeof(line), f))
    {
        char *eq = strchr(line, '=');
        trim(line);
        if (line[0] == '#' || line[0] == ';' || !eq) continue;
        *eq = 0;
        if (!strcmp(line, "mode")) { strncpy(c->mode, eq + 1, sizeof(c->mode) - 1); c->mode[sizeof(c->mode) - 1] = 0; }
        else if (!strcmp(line, "cpu")) { strncpy(c->cpu, eq + 1, sizeof(c->cpu) - 1); c->cpu[sizeof(c->cpu) - 1] = 0; }
        else if (!strcmp(line, "rom")) { strncpy(c->rom, eq + 1, sizeof(c->rom) - 1); c->rom[sizeof(c->rom) - 1] = 0; }
        else if (!strcmp(line, "lastrom")) { strncpy(c->lastRom, eq + 1, sizeof(c->lastRom) - 1); c->lastRom[sizeof(c->lastRom) - 1] = 0; }
        else if (!strcmp(line, "showfps")) c->showFps = atoi(eq + 1);
        else if (!strcmp(line, "menuat")) c->menuAt = (u32)atoi(eq + 1);
        else if (!strcmp(line, "autostart")) c->autoStart = atoi(eq + 1);
        else if (!strcmp(line, "browserat")) c->browserAt = (u32)atoi(eq + 1);
        else if (!strcmp(line, "hle")) c->hle = atoi(eq + 1);
        else if (!strcmp(line, "exitafter")) c->exitAfter = (u32)atoi(eq + 1);
        else if (!strcmp(line, "stateslot")) c->stateSlot = atoi(eq + 1);
        else if (!strcmp(line, "audioms")) c->audioMs = atoi(eq + 1);
        else if (!strcmp(line, "loadstate")) c->loadState = atoi(eq + 1);
        else if (!strcmp(line, "savestateat")) c->saveStateAt = (u32)atoi(eq + 1);
        else if (!strcmp(line, "renderer")) c->softRenderer = !strcmp(eq + 1, "soft");
        else if (!strcmp(line, "trace")) c->trace = (u32)atoi(eq + 1);
        else if (!strcmp(line, "xenia")) c->xenia = atoi(eq + 1);
        else if (!strcmp(line, "fpuflags")) c->fpuFlags = atoi(eq + 1);
        else if (!strcmp(line, "jitfpu")) c->jitFpu = atoi(eq + 1);
        else if (!strcmp(line, "regcache")) c->regCache = atoi(eq + 1);
        else if (!strcmp(line, "smooth")) c->smooth = atoi(eq + 1);
        else if (!strcmp(line, "asyncgfx")) c->asyncGfx = atoi(eq + 1);
        else if (!strcmp(line, "asyncaudio")) c->asyncAudio = atoi(eq + 1);
        else if (!strcmp(line, "audiocycles")) c->audioCycles = (u32)atoi(eq + 1);
        else if (!strcmp(line, "gfxcycles")) c->gfxCycles = (u32)atoi(eq + 1);
        else if (!strcmp(line, "cpi")) c->cpi = atoi(eq + 1);
        else if (!strcmp(line, "xenosdebug")) c->xenosDebug = atoi(eq + 1);
        else if (!strcmp(line, "pauseat")) c->pauseAt = (u32)atoi(eq + 1);
        else if (!strcmp(line, "input")) h64_input_script_parse(&c->input, eq + 1);
        else if (!strcmp(line, "tracestep")) c->traceStep = (u32)strtoul(eq + 1, NULL, 10);
        else if (!strcmp(line, "shots"))
        {
            char *p = eq + 1;
            while (*p && c->shotCount < 64)
            {
                c->shots[c->shotCount++] = (u32)strtoul(p, &p, 10);
                while (*p == ',' || *p == ' ') p++;
            }
        }
    }
    fclose(f);
    return 1;
}

int ConfigWriteMenuKeys(const Config *c, const char *path, int withLastRom)
{
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    fprintf(f, "# Written by the Harissa64 V2 menu\ncpu=%s\nhle=%d\naudioms=%d\nsmooth=%d\nshowfps=%d\n", c->cpu, c->hle, c->audioMs,
            c->smooth, c->showFps);
    if (withLastRom && c->lastRom[0]) fprintf(f, "lastrom=%s\n", c->lastRom);
    fclose(f);
    return 1;
}
