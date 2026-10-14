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
    c->updateCheck = 1;
    c->artDownload = 1;
    c->browserDemo = 0;
    c->browserSel[0] = 0;
    c->artAll = 0;
    c->gameFlags = -1;
    c->logCpu = 5;
    c->menuAt = 0;
    c->aboutAt = 0;
    c->controlsTest = -1;
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
    c->fpCache = 1;
    c->fastFpu = 1;  // host FPU without FCR31 cause/flag tracking, as mupen64plus
    c->fullExits = 0;
    c->superblocks = 1;
    c->smooth = 1;
    c->resScale = 3;
    c->texFilter = 1;
    c->sharpen = 0;
    c->blur = 0;
    c->screen = 0;
    c->brightness = 0;
    c->asyncGfx = 1;
    c->parseWorker = 1;
    c->asyncAudio = 1;
    c->audioCycles = 100000;
    c->gfxCycles = 400000;
    c->gfxTiming = 0;
    c->lateParse = -1;   // per game (main.cpp)
    c->cpi = 2;      // mupen64plus's CountPerOp 1 (its exact setting; its default 2 is our 4)
    c->exitAfter = 0;
    c->audioMs = XB_AUDIO_DEFAULT_MS;
    c->stateSlot = 1;
    c->loadState = 0;
    c->saveStateAt = 0;
    {
        int i;
        for (i = 0; i < 4; i++) h64_padmap_default(&c->padMap[i]);
    }
}

int ConfigParseFile(Config *c, const char *path)
{
    FILE *f = fopen(path, "r");
    static char line[8192];   // input= scripts run long (one line)
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
        else if (!strcmp(line, "updatecheck")) c->updateCheck = atoi(eq + 1);
        else if (!strcmp(line, "artdownload")) c->artDownload = atoi(eq + 1);
        else if (!strcmp(line, "browserdemo")) c->browserDemo = atoi(eq + 1);
        else if (!strcmp(line, "browsersel")) { strncpy(c->browserSel, eq + 1, sizeof(c->browserSel) - 1); c->browserSel[sizeof(c->browserSel) - 1] = 0; }
        else if (!strcmp(line, "artall")) c->artAll = atoi(eq + 1);
        else if (!strcmp(line, "gameflags")) c->gameFlags = (int)strtol(eq + 1, NULL, 16);
        else if (!strcmp(line, "logcpu")) c->logCpu = atoi(eq + 1);
        else if (!strcmp(line, "menuat")) c->menuAt = (u32)atoi(eq + 1);
        else if (!strcmp(line, "aboutat")) c->aboutAt = (u32)atoi(eq + 1);
        else if (!strcmp(line, "controlstest")) c->controlsTest = atoi(eq + 1);
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
        else if (!strcmp(line, "fpcache")) c->fpCache = atoi(eq + 1);
        else if (!strcmp(line, "fastfpu")) c->fastFpu = atoi(eq + 1);
        else if (!strcmp(line, "fullexits")) c->fullExits = atoi(eq + 1);
        else if (!strcmp(line, "superblocks")) c->superblocks = atoi(eq + 1);
        else if (!strcmp(line, "smooth")) c->smooth = atoi(eq + 1);
        else if (!strcmp(line, "resolution")) c->resScale = atoi(eq + 1);
        else if (!strcmp(line, "texfilter")) c->texFilter = atoi(eq + 1);
        else if (!strcmp(line, "sharpen")) c->sharpen = atoi(eq + 1);
        else if (!strcmp(line, "crt")) c->screen = atoi(eq + 1) ? 3 : 0;
        else if (!strcmp(line, "screen")) c->screen = atoi(eq + 1);
        else if (!strcmp(line, "brightness")) c->brightness = atoi(eq + 1);
        else if (!strcmp(line, "blur")) c->blur = atoi(eq + 1);
        else if (!strcmp(line, "asyncgfx")) c->asyncGfx = atoi(eq + 1);
        else if (!strcmp(line, "parseworker")) c->parseWorker = atoi(eq + 1);
        else if (!strcmp(line, "asyncaudio")) c->asyncAudio = atoi(eq + 1);
        else if (!strcmp(line, "audiocycles")) c->audioCycles = (u32)atoi(eq + 1);
        else if (!strcmp(line, "gfxcycles")) c->gfxCycles = (u32)atoi(eq + 1);
        else if (!strcmp(line, "gfxtiming")) c->gfxTiming = atoi(eq + 1);
        else if (!strcmp(line, "lateparse")) c->lateParse = atoi(eq + 1);
        else if (!strcmp(line, "cpi")) c->cpi = atoi(eq + 1);
        else if (!strcmp(line, "xenosdebug")) c->xenosDebug = atoi(eq + 1);
        else if (!strcmp(line, "pauseat")) c->pauseAt = (u32)atoi(eq + 1);
        else if (!strcmp(line, "input")) h64_input_script_parse(&c->input, eq + 1);
        else if (!strcmp(line, "tracestep")) c->traceStep = (u32)strtoul(eq + 1, NULL, 10);
        else if (!strncmp(line, "map", 3)) h64_padmap_parse_key(&c->padMap[0], line, eq + 1);
        else if (line[0] == 'p' && line[1] >= '2' && line[1] <= '4' && line[2] == '_' && !strncmp(line + 3, "map", 3))
            h64_padmap_parse_key(&c->padMap[line[1] - '1'], line + 3, eq + 1);
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
    int i;
    if (!f) return 0;
    fprintf(f, "# Written by the Harissa64 V2 menu\ncpu=%s\nhle=%d\naudioms=%d\nsmooth=%d\nshowfps=%d\n", c->cpu, c->hle, c->audioMs,
            c->smooth, c->showFps);
    fprintf(f, "resolution=%d\ntexfilter=%d\nsharpen=%d\nblur=%d\nscreen=%d\nbrightness=%d\n", c->resScale, c->texFilter,
            c->sharpen, c->blur, c->screen, c->brightness);
    if (withLastRom) fprintf(f, "artdownload=%d\n", c->artDownload);
    // Controls: "map=default" first (a game's profile then undoes custom
    // controls of settings.ini), then every key of a changed mapping.
    for (i = 0; i < 4; i++)
    {
        const H64PadMap *m = &c->padMap[i];
        char pre[8];
        int k;
        pre[0] = 0;
        if (i) sprintf(pre, "p%d_", i + 1);
        fprintf(f, "%smap=default\n", pre);
        if (h64_padmap_is_default(m)) continue;
        for (k = 0; k < H64_N64_COUNT; k++)
        {
            char src[256];
            h64_padmap_format_sources(m->src[k], src, sizeof(src));
            fprintf(f, "%smap_%s=%s\n", pre, h64_padmap_n64_name(k), src);
        }
        fprintf(f, "%smap_stick=%s\n%smap_deadzone=%d\n%smap_range=%d\n", pre,
                m->stick == H64_STICK_RIGHT ? "right" : m->stick == H64_STICK_NONE ? "none" : "left", pre, m->deadzone, pre,
                m->range);
    }
    if (withLastRom && c->lastRom[0]) fprintf(f, "lastrom=%s\n", c->lastRom);
    fclose(f);
    return 1;
}
