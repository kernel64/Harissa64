#include "h64_vi.h"

#include "../common/h64_endian.h"
#include "../system/h64_system.h"

int h64_vi_capture(H64System *sys, u8 *rgb, int maxW, int maxH, int *w, int *h)
{
    u32 status = sys->vi.regs[0], origin = sys->vi.regs[1] & 0xFFFFFF, width = sys->vi.regs[2] & 0xFFF;
    u32 vVideo = sys->vi.regs[10], yScale = sys->vi.regs[13] & 0xFFF;
    int type = (int)(status & 3), bpp, lines, x, y;
    u32 vStart = (vVideo >> 16) & 0x3FF, vEnd = vVideo & 0x3FF;

    if (type < 2 || width == 0)
        return -1;
    bpp = type == 3 ? 4 : 2;
    lines = vEnd > vStart ? (int)((vEnd - vStart) / 2) : 240;
    if (yScale) lines = (int)((u32)lines * yScale / 1024);
    if (lines <= 0) lines = 240;
    if ((int)width > maxW) width = (u32)maxW;
    if (lines > maxH) lines = maxH;

    for (y = 0; y < lines; y++)
        for (x = 0; x < (int)width; x++)
        {
            u32 addr = origin + ((u32)y * (sys->vi.regs[2] & 0xFFF) + (u32)x) * (u32)bpp;
            u8 *out = rgb + ((size_t)y * width + (size_t)x) * 3;
            if (addr + (u32)bpp > H64_RDRAM_SIZE) { out[0] = out[1] = out[2] = 0; continue; }
            if (bpp == 2)
            {
                u16 p = h64_load_be16(sys->rdram + addr);   // RGBA 5551
                out[0] = (u8)(((p >> 11) & 31) << 3);
                out[1] = (u8)(((p >> 6) & 31) << 3);
                out[2] = (u8)(((p >> 1) & 31) << 3);
            }
            else
            {
                out[0] = sys->rdram[addr];
                out[1] = sys->rdram[addr + 1];
                out[2] = sys->rdram[addr + 2];
            }
        }
    *w = (int)width;
    *h = lines;
    return 0;
}
