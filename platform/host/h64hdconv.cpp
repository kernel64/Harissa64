// h64hdconv: converts a community N64 hi-res texture pack (GLideN64 / Rice
// PNG names: NAME#TEXCRC#FMT#SIZE[#PALCRC]_suffix.png) into a Harissa64 HD
// file (core/rdp/h64_hdpack.h) the Xbox 360 front end reads.
//
//   h64hdconv PACK_DIR ROM_NAME OUT.h64hd [--max N] [--skip TEXT]...
//
// PACK_DIR: the pack's extracted folder (searched recursively). ROM_NAME: the
// ROM's internal name as the PNG names start ("SUPER MARIO 64"; the case is
// ignored). --max N: textures larger than N pixels on a side are halved until
// they fit (default 1024). --skip TEXT: ignore paths containing TEXT (several
// allowed; default "Hacks/", "iQue/", "Ports/": other ROMs and other emulators).
//
// PNG decoding: stb_image; DXT compression: stb_dxt (third_party/stb, public domain).
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "../../third_party/stb/stb_image.h"
#define STB_DXT_IMPLEMENTATION
#include "../../third_party/stb/stb_dxt.h"
#include "../../core/rdp/h64_hdpack.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct Item
{
    H64HdEntry e;
    std::vector<unsigned char> data;
};

static std::string upper(std::string s)
{
    for (size_t i = 0; i < s.size(); i++) s[i] = (char)toupper((unsigned char)s[i]);
    return s;
}

// NAME#TEX#FMT#SIZE[#PAL]_suffix -> key; 0 if the name is not a Rice name for this ROM.
static int parse_name(const std::string &file, const std::string &romUpper, H64HdEntry *e)
{
    std::string base = file;
    size_t dot = base.rfind('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    size_t us = base.rfind('_');
    if (us != std::string::npos && base.find('#', us) == std::string::npos) base = base.substr(0, us);
    std::vector<std::string> p;
    size_t s = 0, h;
    while ((h = base.find('#', s)) != std::string::npos) { p.push_back(base.substr(s, h - s)); s = h + 1; }
    p.push_back(base.substr(s));
    if (p.size() < 4 || upper(p[0]) != romUpper) return 0;
    memset(e, 0, sizeof(*e));
    if (p[1] == "$") e->flags |= H64HD_WILDCARD;
    else e->texCrc = (u32)strtoul(p[1].c_str(), 0, 16);
    e->fmt = (u8)atoi(p[2].c_str());
    e->size = (u8)atoi(p[3].c_str());
    if (p.size() > 4 && !p[4].empty()) { e->palCrc = (u32)strtoul(p[4].c_str(), 0, 16); e->flags |= H64HD_HAS_PAL; }
    if ((e->flags & H64HD_WILDCARD) && !(e->flags & H64HD_HAS_PAL)) return 0;
    return 1;
}

static bool key_less(const H64HdEntry &a, const H64HdEntry &b)
{
    if (a.texCrc != b.texCrc) return a.texCrc < b.texCrc;
    if (a.palCrc != b.palCrc) return a.palCrc < b.palCrc;
    if (a.fmt != b.fmt) return a.fmt < b.fmt;
    if (a.size != b.size) return a.size < b.size;
    return a.flags < b.flags;
}

static bool key_eq(const H64HdEntry &a, const H64HdEntry &b)
{
    return a.texCrc == b.texCrc && a.palCrc == b.palCrc && a.fmt == b.fmt && a.size == b.size && a.flags == b.flags;
}

// Half size (box filter); w, h even.
static std::vector<unsigned char> half(const std::vector<unsigned char> &src, int w, int h)
{
    std::vector<unsigned char> d((size_t)(w / 2) * (h / 2) * 4);
    for (int y = 0; y < h / 2; y++)
        for (int x = 0; x < w / 2; x++)
            for (int c = 0; c < 4; c++)
            {
                int s = src[((2 * y) * w + 2 * x) * 4 + c] + src[((2 * y) * w + 2 * x + 1) * 4 + c] +
                        src[((2 * y + 1) * w + 2 * x) * 4 + c] + src[((2 * y + 1) * w + 2 * x + 1) * 4 + c];
                d[(y * (w / 2) + x) * 4 + c] = (unsigned char)((s + 2) / 4);
            }
    return d;
}

// One mip level's DXT blocks, 16-bit words byte-swapped for the console.
static void compress(const std::vector<unsigned char> &rgba, int w, int h, int dxt5, std::vector<unsigned char> &out)
{
    unsigned char block[64], dst[16];
    for (int by = 0; by < h; by += 4)
        for (int bx = 0; bx < w; bx += 4)
        {
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++)
                    memcpy(block + (y * 4 + x) * 4, &rgba[((by + y) * w + bx + x) * 4], 4);
            stb_compress_dxt_block(dst, block, dxt5, STB_DXT_HIGHQUAL);
            int n = dxt5 ? 16 : 8;
            for (int i = 0; i < n; i += 2) { out.push_back(dst[i + 1]); out.push_back(dst[i]); }
        }
}

static void put32(std::vector<unsigned char> &b, u32 v) { b.push_back((unsigned char)(v >> 24)); b.push_back((unsigned char)(v >> 16)); b.push_back((unsigned char)(v >> 8)); b.push_back((unsigned char)v); }
static void put16(std::vector<unsigned char> &b, u32 v) { b.push_back((unsigned char)(v >> 8)); b.push_back((unsigned char)v); }

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        fprintf(stderr, "usage: h64hdconv PACK_DIR ROM_NAME OUT.h64hd [--max N] [--skip TEXT]...\n");
        return 2;
    }
    std::string dir = argv[1], romUpper = upper(argv[2]), outPath = argv[3];
    int maxSide = 1024;
    std::vector<std::string> skips;
    for (int i = 4; i < argc; i++)
    {
        if (!strcmp(argv[i], "--max") && i + 1 < argc) maxSide = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--skip") && i + 1 < argc) skips.push_back(argv[++i]);
    }
    if (skips.empty()) { skips.push_back("Hacks/"); skips.push_back("iQue/"); skips.push_back("Ports/"); }

    std::vector<Item> items;
    size_t seen = 0, failed = 0, dup = 0;
    for (const fs::directory_entry &de : fs::recursive_directory_iterator(dir))
    {
        if (!de.is_regular_file()) continue;
        std::string path = de.path().generic_string(), name = de.path().filename().string();
        if (upper(de.path().extension().string()) != ".PNG") continue;
        bool skip = false;
        for (size_t k = 0; k < skips.size(); k++) if (path.find(skips[k]) != std::string::npos) skip = true;
        if (skip) continue;
        Item it;
        if (!parse_name(name, romUpper, &it.e)) continue;
        seen++;
        int w, h, n;
        unsigned char *px = stbi_load(path.c_str(), &w, &h, &n, 4);
        if (!px) { failed++; fprintf(stderr, "cannot read %s\n", path.c_str()); continue; }
        std::vector<unsigned char> img(px, px + (size_t)w * h * 4);
        stbi_image_free(px);
        // Fit the size limit (halving keeps the aspect), then pad to a multiple of 4 by repeating the edge.
        while ((w > maxSide || h > maxSide) && w % 2 == 0 && h % 2 == 0) { img = half(img, w, h); w /= 2; h /= 2; }
        int pw = (w + 3) & ~3, ph = (h + 3) & ~3;
        if (pw != w || ph != h)
        {
            std::vector<unsigned char> p((size_t)pw * ph * 4);
            for (int y = 0; y < ph; y++)
                for (int x = 0; x < pw; x++)
                    memcpy(&p[((size_t)y * pw + x) * 4], &img[((size_t)std::min(y, h - 1) * w + std::min(x, w - 1)) * 4], 4);
            img.swap(p);
            w = pw; h = ph;
        }
        int alpha = 0;
        for (size_t i = 3; i < img.size(); i += 4) if (img[i] < 250) { alpha = 1; break; }
        it.e.dxt = alpha ? 5 : 1;
        it.e.width = (u16)w;
        it.e.height = (u16)h;
        // Mip levels while both sides stay even multiples of 4.
        int levels = 0, lw = w, lh = h;
        std::vector<unsigned char> lv = img;
        for (;;)
        {
            compress(lv, lw, lh, alpha, it.data);
            levels++;
            if (lw % 8 || lh % 8 || levels >= 12) break;
            lv = half(lv, lw, lh);
            lw /= 2; lh /= 2;
        }
        it.e.mips = (u8)levels;
        items.push_back(it);
        if (items.size() % 200 == 0) fprintf(stderr, "%zu textures...\n", items.size());
    }
    std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return key_less(a.e, b.e); });
    std::vector<Item> uniq;
    for (size_t i = 0; i < items.size(); i++)
    {
        if (!uniq.empty() && key_eq(uniq.back().e, items[i].e)) { dup++; continue; }
        uniq.push_back(items[i]);
    }

    u32 count = (u32)uniq.size(), dataOff = H64HD_HEADER_BYTES + count * H64HD_ENTRY_BYTES, off = dataOff;
    std::vector<unsigned char> head;
    head.insert(head.end(), H64HD_MAGIC, H64HD_MAGIC + 8);
    put32(head, H64HD_VERSION); put32(head, count); put32(head, H64HD_HEADER_BYTES); put32(head, dataOff); put32(head, 0); put32(head, 0);
    for (size_t i = 0; i < uniq.size(); i++)
    {
        H64HdEntry &e = uniq[i].e;
        e.offset = off;
        e.bytes = (u32)uniq[i].data.size();
        off += (e.bytes + 127) & ~127u;   // 128-byte aligned data
        put32(head, e.texCrc); put32(head, e.palCrc);
        head.push_back(e.fmt); head.push_back(e.size); head.push_back(e.flags); head.push_back(e.dxt);
        put16(head, e.width); put16(head, e.height);
        head.push_back(e.mips); head.push_back(0); put16(head, 0);
        put32(head, e.offset); put32(head, e.bytes); put32(head, 0);
    }
    FILE *f = fopen(outPath.c_str(), "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", outPath.c_str()); return 1; }
    fwrite(head.data(), 1, head.size(), f);
    static const unsigned char zeros[128] = { 0 };
    for (size_t i = 0; i < uniq.size(); i++)
    {
        fwrite(uniq[i].data.data(), 1, uniq[i].data.size(), f);
        size_t pad = ((uniq[i].data.size() + 127) & ~(size_t)127) - uniq[i].data.size();
        if (pad) fwrite(zeros, 1, pad, f);
    }
    fclose(f);
    printf("%s: %u textures (%zu PNG names, %zu duplicates, %zu unreadable), %.1f MB\n", outPath.c_str(), count, seen, dup, failed,
           off / 1048576.0);
    return 0;
}
