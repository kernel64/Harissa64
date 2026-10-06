#include "h64_ppc_emit.h"

void ppc_li64(H64PpcCode *c, u32 rt, u64 v)
{
    s64 sv = (s64)v;
    if (sv >= -32768 && sv <= 32767)
    {
        ppc_li(c, rt, (s32)sv);
        return;
    }
    if (sv >= -0x80000000ll && sv <= 0x7FFFFFFFll)
    {
        ppc_lis(c, rt, (s32)(s16)(v >> 16));
        if (v & 0xFFFF) ppc_ori(c, rt, rt, (u32)v & 0xFFFF);
        return;
    }
    // Upper 32 bits, shift, then the lower 32 bits.
    ppc_lis(c, rt, (s32)(s16)(v >> 48));
    if ((v >> 32) & 0xFFFF) ppc_ori(c, rt, rt, (u32)(v >> 32) & 0xFFFF);
    ppc_sldi(c, rt, rt, 32);
    if ((v >> 16) & 0xFFFF) ppc_oris(c, rt, rt, (u32)(v >> 16) & 0xFFFF);
    if (v & 0xFFFF) ppc_ori(c, rt, rt, (u32)v & 0xFFFF);
}

void ppc_li32u(H64PpcCode *c, u32 rt, u32 v)
{
    if (v < 0x8000)
    {
        ppc_li(c, rt, (s32)v);
        return;
    }
    ppc_lis(c, rt, (s32)(s16)(v >> 16));
    if (v & 0xFFFF) ppc_ori(c, rt, rt, v & 0xFFFF);
    if (v & 0x80000000u) ppc_clrldi(c, rt, rt, 32);   // lis sign-extends
}
