#include "h64_mem.h"

#include <stdlib.h>

#if defined(_XBOX)
#include <xtl.h>

// Blocks from XPhysicalAlloc, to give back with XPhysicalFree (others: free).
static void *s_physical[16];

static void remember(void *p)
{
    int i;
    for (i = 0; i < 16; i++)
        if (!s_physical[i]) { s_physical[i] = p; return; }
}

void *h64_big_alloc(size_t bytes)
{
    const size_t mb16 = 16u << 20, kb64 = 64u << 10;
    void *p = 0;
    int i;
    for (i = 0; i < 16 && s_physical[i]; i++) {}
    if (i < 16)
    {
        // 16 MB pages for blocks of 8 MB or more (RDRAM: one TLB entry),
        // else 64 KB pages.
        if (bytes >= (8u << 20))
            p = XPhysicalAlloc((bytes + mb16 - 1) & ~(mb16 - 1), MAXULONG_PTR, mb16, PAGE_READWRITE | MEM_16MB_PAGES);
        if (!p) p = XPhysicalAlloc((bytes + kb64 - 1) & ~(kb64 - 1), MAXULONG_PTR, kb64, PAGE_READWRITE | MEM_LARGE_PAGES);
        if (p)
        {
            remember(p);
            return p;
        }
    }
    return malloc(bytes);
}

void h64_big_free(void *p)
{
    int i;
    if (!p) return;
    for (i = 0; i < 16; i++)
        if (s_physical[i] == p)
        {
            s_physical[i] = 0;
            XPhysicalFree(p);
            return;
        }
    free(p);
}

#else

void *h64_big_alloc(size_t bytes) { return malloc(bytes); }
void h64_big_free(void *p) { free(p); }

#endif
