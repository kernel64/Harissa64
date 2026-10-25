// Harissa64 V2 - the console's drives (Xbox 360), see xb_drives.h.
#include <xtl.h>
#include <stdio.h>
#include <string.h>

#include "xb_drives.h"
#include "../../core/common/h64_log.h"

// Kernel exports (xboxkrnl.lib) the XDK headers do not declare: a symbolic
// link "\??\Usb0:" -> "\Device\Mass0" makes "Usb0:\..." paths work for the
// file functions of this title.
extern "C" {
typedef struct _H64_OB_STRING
{
    USHORT Length;
    USHORT MaximumLength;
    PCHAR Buffer;
} H64_OB_STRING;
LONG ObCreateSymbolicLink(H64_OB_STRING *link, H64_OB_STRING *device);
}

struct DriveDef { const char *name, *device; };
static const DriveDef s_drives[] = {
    { "Hdd1", "\\Device\\Harddisk0\\Partition1" },   // the hard disk's content partition
    { "Usb0", "\\Device\\Mass0" },                    // USB storage, ports in order
    { "Usb1", "\\Device\\Mass1" },
    { "Usb2", "\\Device\\Mass2" },
    { "Dvd", "\\Device\\Cdrom0" },
};

static void ob_string(H64_OB_STRING *s, char *text)
{
    s->Length = (USHORT)strlen(text);
    s->MaximumLength = (USHORT)(s->Length + 1);
    s->Buffer = text;
}

void DrivesMount(void)
{
    int i;
    for (i = 0; i < (int)(sizeof(s_drives) / sizeof(s_drives[0])); i++)
    {
        char link[32], device[64];
        H64_OB_STRING l, d;
        LONG st;
        _snprintf(link, sizeof(link), "\\??\\%s:", s_drives[i].name);
        link[sizeof(link) - 1] = 0;
        strncpy(device, s_drives[i].device, sizeof(device) - 1);
        device[sizeof(device) - 1] = 0;
        ob_string(&l, link);
        ob_string(&d, device);
        st = ObCreateSymbolicLink(&l, &d);
        // 0: made; anything else (already there from an earlier start, or
        // no such device) is logged and left alone.
        H64_INFO("[drives] %s: -> %s: %08X", s_drives[i].name, s_drives[i].device, (unsigned)st);
    }
}

int DrivesList(char roots[][16], int max)
{
    int n = 0, i;
    if (n < max) strcpy(roots[n++], "game:\\");
    for (i = 0; i < (int)(sizeof(s_drives) / sizeof(s_drives[0])) && n < max; i++)
    {
        char root[16];
        _snprintf(root, sizeof(root), "%s:\\", s_drives[i].name);
        root[sizeof(root) - 1] = 0;
        // Present when its root can be listed, even empty (an empty USB port
        // or DVD tray fails with "path not found" / "not ready").
        {
            char pattern[20];
            WIN32_FIND_DATAA fd;
            HANDLE h;
            _snprintf(pattern, sizeof(pattern), "%s*", root);
            pattern[sizeof(pattern) - 1] = 0;
            h = FindFirstFileA(pattern, &fd);
            if (h != INVALID_HANDLE_VALUE)
            {
                FindClose(h);
                strcpy(roots[n++], root);
            }
            else if (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_NO_MORE_FILES)
                strcpy(roots[n++], root);
        }
    }
    return n;
}
