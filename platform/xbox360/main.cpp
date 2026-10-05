// Harissa64 V2 - Xbox 360 entry point (M0 skeleton).
//
// Runs the unit tests on the console's own CPU at start-up, writes them to
// game:\harissa64v2.log (cache:\ when game:\ is read-only, as in Xenia), and
// shows the version and the result on screen until Back is pressed.
//
// Text is drawn with D3D Clear rectangles from an 8x8 bitmap font, so this
// skeleton needs no shaders or font files.
#include <xtl.h>
#include <stdio.h>
#include <string.h>

#include "../../core/common/h64_types.h"
#include "../../core/common/h64_log.h"
#include "../../core/common/h64_version.h"
#include "../../tests/unit/unit_tests.h"
#include "font8x8_basic.h"

static FILE *s_log = NULL;

static void file_sink(int level, const char *line)
{
    OutputDebugStringA(line);
    OutputDebugStringA("\n");
    if (s_log)
    {
        fprintf(s_log, "%s\n", line);
        fflush(s_log);
    }
}

// ---- Text through Clear rectangles ----
#define MAX_RECTS 4096
static D3DRECT s_rects[MAX_RECTS];
static int s_rectCount;

static void AddText(int x, int y, int scale, const char *text)
{
    int cx = x;
    for (; *text; text++)
    {
        unsigned char c = (unsigned char)*text;
        int row, col;
        if (c == '\n') { y += 10 * scale; cx = x; continue; }
        if (c >= 128) c = '?';
        for (row = 0; row < 8; row++)
        {
            unsigned char bits = font8x8_basic[c][row];
            for (col = 0; col < 8; col++)
            {
                if ((bits & (1 << col)) && s_rectCount < MAX_RECTS)   // bit 0 = leftmost pixel
                {
                    D3DRECT *r = &s_rects[s_rectCount++];
                    r->x1 = cx + col * scale;
                    r->y1 = y + row * scale;
                    r->x2 = r->x1 + scale;
                    r->y2 = r->y1 + scale;
                }
            }
        }
        cx += 8 * scale;
    }
}

static void FlushText(IDirect3DDevice9 *dev, D3DCOLOR color)
{
    if (s_rectCount)
        dev->Clear(s_rectCount, s_rects, D3DCLEAR_TARGET, color, 1.0f, 0);
    s_rectCount = 0;
}

int __cdecl main()
{
    IDirect3D9 *d3d;
    IDirect3DDevice9 *dev = NULL;
    D3DPRESENT_PARAMETERS pp;
    int tests = 0, checks = 0, failures;
    char status[256];

    s_log = fopen("game:\\harissa64v2.log", "w");
    if (!s_log)
        s_log = fopen("cache:\\harissa64v2.log", "w");
    h64_log_set_sink(file_sink);
    H64_INFO("[main] Harissa64 V2 %s (Xbox 360), %s-endian, %d-bit pointers", H64_VERSION_STRING,
             H64_HOST_BIG_ENDIAN ? "big" : "little", (int)(sizeof(void *) * 8));

    failures = h64_run_all_unit_tests(&tests, &checks);
    _snprintf(status, sizeof(status), "Unit tests: %s\n%d tests, %d checks, %d failures",
              failures ? "FAILED" : "PASSED", tests, checks, failures);
    status[sizeof(status) - 1] = 0;
    H64_INFO("[main] UNIT %s: %d tests, %d checks, %d failures", failures ? "FAILED" : "PASSED", tests, checks,
             failures);

    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    ZeroMemory(&pp, sizeof(pp));
    pp.BackBufferWidth = 1280;
    pp.BackBufferHeight = 720;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    if (!d3d || FAILED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, NULL, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev)))
    {
        H64_ERROR("[main] CreateDevice failed");
        dev = NULL;
    }

    for (;;)
    {
        XINPUT_STATE in;
        if (dev)
        {
            char line[128];
            dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(14, 16, 26), 1.0f, 0);
            AddText(96, 80, 6, "HARISSA64 V2");
            FlushText(dev, D3DCOLOR_XRGB(220, 40, 30));
            _snprintf(line, sizeof(line), "Version %s", H64_VERSION_STRING);
            line[sizeof(line) - 1] = 0;
            AddText(100, 170, 3, line);
            _snprintf(line, sizeof(line), "%s-endian, %d-bit pointers", H64_HOST_BIG_ENDIAN ? "Big" : "Little",
                      (int)(sizeof(void *) * 8));
            line[sizeof(line) - 1] = 0;
            AddText(100, 220, 3, line);
            FlushText(dev, D3DCOLOR_XRGB(230, 230, 230));
            AddText(100, 300, 3, status);
            FlushText(dev, failures ? D3DCOLOR_XRGB(240, 60, 60) : D3DCOLOR_XRGB(60, 220, 90));
            AddText(100, 600, 2, "Press BACK to return to the dashboard");
            FlushText(dev, D3DCOLOR_XRGB(140, 140, 160));
            dev->Present(NULL, NULL, NULL, NULL);
        }
        if (XInputGetState(0, &in) == ERROR_SUCCESS && (in.Gamepad.wButtons & XINPUT_GAMEPAD_BACK))
            break;
        Sleep(16);
    }

    if (s_log)
        fclose(s_log);
    XLaunchNewImage(XLAUNCH_KEYWORD_DEFAULT_APP, 0);
    return 0;
}
