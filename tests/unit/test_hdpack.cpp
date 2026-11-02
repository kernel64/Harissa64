// Hi-res texture packs: Rice names (core/rdp/h64_hdbuild.cpp), the PNG to DXT
// conversion on two tiny pictures made by Python's zlib, and the catalogue
// (core/cart/h64_hdcatalog.cpp). Nothing is written to disk.
#include <stdlib.h>
#include <string.h>

#include "../../core/cart/h64_hdcatalog.h"
#include "../../core/common/h64_test.h"
#include "../../core/rdp/h64_hdbuild.h"

// 8x8 RGBA, the top-left pixel transparent: DXT5, levels 8x8 and 4x4.
static const u8 s_png8[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x08, 0x08, 0x06, 0x00, 0x00, 0x00, 0xC4, 0x0F, 0xBE,
    0x8B, 0x00, 0x00, 0x00, 0x5F, 0x49, 0x44, 0x41, 0x54, 0x78, 0xDA, 0x15, 0xCA, 0x31, 0x01, 0xC0,
    0x30, 0x08, 0x00, 0xB0, 0x4A, 0xA9, 0x14, 0xA4, 0x20, 0x05, 0x29, 0x48, 0x41, 0x0A, 0x4E, 0xB6,
    0xF4, 0xC8, 0x97, 0x73, 0x4E, 0x9D, 0x7B, 0xEA, 0x0B, 0x92, 0xA2, 0x19, 0x96, 0x73, 0x6E, 0x7D,
    0x97, 0x20, 0x29, 0x9A, 0x61, 0xEF, 0x0B, 0x21, 0x10, 0x24, 0x45, 0x33, 0x6C, 0xBC, 0x90, 0x02,
    0x41, 0x52, 0x34, 0xC3, 0xE6, 0x0B, 0x25, 0x10, 0x24, 0x45, 0x33, 0x6C, 0xBD, 0xD0, 0x02, 0x41,
    0x52, 0x34, 0xC3, 0xF6, 0x0B, 0x23, 0x10, 0x24, 0x45, 0x33, 0xEC, 0xBC, 0xB0, 0x02, 0x41, 0x52,
    0x34, 0xC3, 0xF2, 0x03, 0xF7, 0x86, 0x96, 0xC2, 0x7A, 0x93, 0xA3, 0x87, 0x00, 0x00, 0x00, 0x00,
    0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82,
};
// 4x4 opaque, one colour: DXT1, one level.
static const u8 s_png4[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x04, 0x08, 0x06, 0x00, 0x00, 0x00, 0xA9, 0xF1, 0x9E,
    0x7E, 0x00, 0x00, 0x00, 0x12, 0x49, 0x44, 0x41, 0x54, 0x78, 0xDA, 0x63, 0x38, 0x91, 0x62, 0xF4,
    0x1F, 0x19, 0x33, 0x90, 0x2E, 0x00, 0x00, 0x02, 0x97, 0x25, 0xD1, 0x99, 0x13, 0x12, 0xB7, 0x00,
    0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82,
};

static const char s_catalog[] =
    "{ \"format\": \"1\", \"about\": \"test\",\n"
    "  \"sm64\": { \"name\": \"SM64 Reloaded\", \"games\": \"NSM\", \"rom\": \"SUPER MARIO 64\", \"author\": \"A\",\n"
    "            \"url\": \"https://example.org/a.zip\", \"size\": \"280611003\",\n"
    "            \"sha256\": \"e4aeb971e01cce94ed0ecc1e1e9c79c71c3ed69c9c2225e96567b5c0dd120974\" },\n"
    "  \"two\": { \"name\": \"Two\", \"games\": \"NAB, NCD\", \"rom\": \"TWO\", \"url\": \"https://example.org/b.zip\", \"size\": \"10\" },\n"
    "  \"http\": { \"name\": \"Plain\", \"games\": \"NXX\", \"rom\": \"X\", \"url\": \"http://example.org/c.zip\", \"size\": \"10\" },\n"
    "  \"nosize\": { \"name\": \"No size\", \"games\": \"NXX\", \"rom\": \"X\", \"url\": \"https://example.org/d.zip\" } }\n";

void test_hdpack(H64TestContext *ctx)
{
    H64HdEntry e;
    H64HdItem it;
    std::vector<H64HdPack> packs;

    // Rice names: key, palette, wildcard, suffixes, other ROMs.
    H64_CHECK(ctx, h64_hdbuild_parse("SUPER MARIO 64#3CC6DF3A#0#2_all.png", "Super Mario 64", &e) == 1);
    H64_CHECK_EQ(ctx, e.texCrc, 0x3CC6DF3Au);
    H64_CHECK_EQ(ctx, (u32)e.fmt, 0u);
    H64_CHECK_EQ(ctx, (u32)e.size, 2u);
    H64_CHECK_EQ(ctx, (u32)e.flags, 0u);
    H64_CHECK(ctx, h64_hdbuild_parse("MARIOKART64#1234ABCD#2#1#00FF00FF_ciByRGBA.png", "MARIOKART64", &e) == 1);
    H64_CHECK_EQ(ctx, e.palCrc, 0x00FF00FFu);
    H64_CHECK_EQ(ctx, (u32)e.flags, (u32)H64HD_HAS_PAL);
    H64_CHECK(ctx, h64_hdbuild_parse("MARIOKART64#$#2#1#00FF00FF_all.png", "MARIOKART64", &e) == 1);
    H64_CHECK_EQ(ctx, (u32)e.flags, (u32)(H64HD_HAS_PAL | H64HD_WILDCARD));
    H64_CHECK(ctx, h64_hdbuild_parse("MARIOKART64#$#2#1_all.png", "MARIOKART64", &e) == 0);   // a wildcard needs a palette
    H64_CHECK(ctx, h64_hdbuild_parse("ZELDA MAJORA'S MASK#0BB0AA74#0#2_all.png", "ZELDA MAJORA'S MASK", &e) == 1);
    H64_CHECK(ctx, h64_hdbuild_parse("SUPER MARIO 64#3CC6DF3A#0#2_all.png", "MARIOKART64", &e) == 0);
    H64_CHECK(ctx, h64_hdbuild_parse("0689a6dd#0#2.png", "MARIOKART64", &e) == 0);
    H64_CHECK(ctx, h64_hdbuild_skip("MARIOKART64/Ports/Dolphin/x.png") == 1);
    H64_CHECK(ctx, h64_hdbuild_skip("MARIOKART64/HUD/x.png") == 0);

    // PNG to DXT.
    memset(&it, 0, sizeof(it));
    H64_CHECK(ctx, h64_hdbuild_make(s_png8, sizeof(s_png8), 1024, &it) == 0);
    H64_CHECK_EQ(ctx, (u32)it.e.dxt, 5u);
    H64_CHECK_EQ(ctx, (u32)it.e.width, 8u);
    H64_CHECK_EQ(ctx, (u32)it.e.mips, 2u);
    H64_CHECK_EQ(ctx, it.bytes, 4u * 16 + 16);
    free(it.data);
    memset(&it, 0, sizeof(it));
    H64_CHECK(ctx, h64_hdbuild_make(s_png8, sizeof(s_png8), 4, &it) == 0);   // halved to fit 4
    H64_CHECK_EQ(ctx, (u32)it.e.width, 4u);
    H64_CHECK_EQ(ctx, (u32)it.e.mips, 1u);
    free(it.data);
    memset(&it, 0, sizeof(it));
    H64_CHECK(ctx, h64_hdbuild_make(s_png4, sizeof(s_png4), 1024, &it) == 0);
    H64_CHECK_EQ(ctx, (u32)it.e.dxt, 1u);
    H64_CHECK_EQ(ctx, it.bytes, 8u);
    free(it.data);
    H64_CHECK(ctx, h64_hdbuild_make(s_png4, 20, 1024, &it) == -1);   // cut short

    // Catalogue: valid packs only, game codes without the region.
    H64_CHECK_EQ(ctx, h64_hdcat_parse(s_catalog, (u32)strlen(s_catalog), &packs), 2);
    if (packs.size() == 2)
    {
        H64_CHECK(ctx, packs[0].id == "sm64" && packs[0].rom == "SUPER MARIO 64" && packs[0].size == 280611003u);
        H64_CHECK(ctx, h64_hdcat_fits(&packs[0], "NSME") == 1);
        H64_CHECK(ctx, h64_hdcat_fits(&packs[0], "NSMP") == 1);
        H64_CHECK(ctx, h64_hdcat_fits(&packs[0], "NKTE") == 0);
        H64_CHECK(ctx, h64_hdcat_fits(&packs[1], "NCDJ") == 1);
        H64_CHECK(ctx, h64_hdcat_fits(&packs[1], "NABE") == 1);
        // A hack: another code, the same cartridge name (any case, spaces around).
        H64_CHECK(ctx, h64_hdcat_fits_rom(&packs[0], "NXYE", "super mario 64 ") == 1);
        H64_CHECK(ctx, h64_hdcat_fits_rom(&packs[0], "NSME", "SM64 HACK") == 1);
        H64_CHECK(ctx, h64_hdcat_fits_rom(&packs[0], "NXYE", "SUPER MARIO 6") == 0);
        H64_CHECK(ctx, h64_hdcat_fits_rom(&packs[0], NULL, "") == 0);
    }
    H64_CHECK_EQ(ctx, h64_hdcat_parse("{\"a\": 1}", 8, &packs), -1);
}
