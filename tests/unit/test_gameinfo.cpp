// Unit tests: PNG decoding (core/common/h64_png) and game information
// matching (core/cart/h64_gameinfo), for the ROM browser's box art.
#include <stdlib.h>
#include <string.h>

#include "../../core/cart/h64_gameinfo.h"
#include "../../core/common/h64_png.h"
#include "../../core/common/h64_test.h"

// Test pictures made by a script from known pixels: RGB 8-bit with every
// filter type, RGBA 16-bit, a 2-bit palette with transparency and Adam7.
static const u8 s_pngRgb[163] = {
    137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,6,
    0,0,0,5,8,2,0,0,0,233,58,10,177,0,0,0,106,73,68,65,
    84,120,218,1,95,0,160,255,0,165,77,202,24,37,48,187,29,109,19,44,
    222,214,35,123,46,217,30,1,63,114,31,140,167,82,76,43,35,191,5,168,
    199,19,248,195,98,253,2,225,172,74,51,193,47,215,164,37,195,54,32,223,
    205,101,157,241,180,3,131,22,8,14,213,65,107,92,35,161,107,7,102,139,
    98,20,114,62,4,247,205,229,73,172,53,114,203,205,190,197,21,168,79,254,
    112,76,100,107,32,41,231,104,229,62,162,0,0,0,0,73,69,78,68,174,
    66,96,130,
};
static const u8 s_pngRgbExp[120] = {
    165,77,202,255,24,37,48,255,187,29,109,255,19,44,222,255,214,35,123,255,
    46,217,30,255,63,114,31,255,203,25,113,255,23,68,148,255,214,73,60,255,
    157,92,52,255,96,190,49,255,32,30,105,255,254,218,160,255,238,232,185,255,
    153,127,92,255,124,41,153,255,253,175,229,255,147,37,60,255,214,84,175,255,
    77,250,215,255,20,39,160,255,174,179,254,255,233,35,47,255,138,242,33,255,
    31,158,228,255,145,197,177,255,11,236,181,255,86,59,252,255,30,111,147,255,
};
static const u8 s_pngRgba16[118] = {
    137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,3,
    0,0,0,2,16,6,0,0,0,205,228,186,89,0,0,0,61,73,68,65,
    84,120,218,1,50,0,205,255,4,66,56,126,199,203,185,200,42,188,254,171,
    122,138,116,29,209,207,110,101,255,241,238,247,114,3,109,113,149,62,82,84,
    94,180,176,196,207,212,164,14,135,87,172,18,9,129,63,44,107,218,177,67,
    25,249,56,128,79,189,0,0,0,0,73,69,78,68,174,66,96,130,
};
static const u8 s_pngRgba16Exp[24] = {
    66,126,203,200,254,41,85,229,205,142,70,220,142,212,183,194,118,77,42,90,
    77,118,119,6,
};
static const u8 s_pngPal[152] = {
    137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,9,
    0,0,0,9,2,3,0,0,1,234,248,222,21,0,0,0,12,80,76,84,
    69,10,20,30,200,100,50,0,255,0,255,255,255,90,14,131,17,0,0,0,
    2,116,82,78,83,255,128,8,15,179,106,0,0,0,57,73,68,65,84,120,
    156,99,120,192,248,129,145,129,169,129,233,13,243,6,150,11,12,14,44,87,
    87,51,156,96,96,72,99,228,97,74,96,86,96,145,97,244,95,123,132,105,
    147,74,3,243,191,230,112,150,21,193,7,0,93,68,15,33,71,6,241,230,
    0,0,0,0,73,69,78,68,174,66,96,130,
};
static const u8 s_pngPalExp[324] = {
    255,255,255,255,200,100,50,128,0,255,0,255,0,255,0,255,10,20,30,255,
    200,100,50,128,255,255,255,255,0,255,0,255,0,255,0,255,200,100,50,128,
    10,20,30,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,10,20,30,255,255,255,255,255,255,255,255,255,10,20,30,255,
    200,100,50,128,10,20,30,255,200,100,50,128,255,255,255,255,200,100,50,128,
    10,20,30,255,0,255,0,255,10,20,30,255,10,20,30,255,10,20,30,255,
    200,100,50,128,10,20,30,255,0,255,0,255,10,20,30,255,10,20,30,255,
    200,100,50,128,255,255,255,255,200,100,50,128,0,255,0,255,0,255,0,255,
    0,255,0,255,255,255,255,255,10,20,30,255,10,20,30,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,0,255,0,255,10,20,30,255,
    200,100,50,128,10,20,30,255,0,255,0,255,0,255,0,255,255,255,255,255,
    200,100,50,128,10,20,30,255,200,100,50,128,0,255,0,255,200,100,50,128,
    10,20,30,255,0,255,0,255,10,20,30,255,0,255,0,255,0,255,0,255,
    200,100,50,128,0,255,0,255,200,100,50,128,0,255,0,255,200,100,50,128,
    200,100,50,128,200,100,50,128,255,255,255,255,200,100,50,128,200,100,50,128,
    255,255,255,255,0,255,0,255,10,20,30,255,10,20,30,255,0,255,0,255,
    255,255,255,255,
};

static void check_png(H64TestContext *ctx, const u8 *png, u32 size, const u8 *expected, int w, int h)
{
    int gw = 0, gh = 0;
    u8 *px = h64_png_decode(png, size, &gw, &gh, 1u << 20);
    H64_CHECK(ctx, px != NULL);
    if (!px) return;
    H64_CHECK_EQ(ctx, gw, w);
    H64_CHECK_EQ(ctx, gh, h);
    H64_CHECK(ctx, memcmp(px, expected, (size_t)w * h * 4) == 0);
    free(px);
}

void test_png(H64TestContext *ctx)
{
    int w, h;
    u8 *bad;
    check_png(ctx, s_pngRgb, sizeof(s_pngRgb), s_pngRgbExp, 6, 5);
    check_png(ctx, s_pngRgba16, sizeof(s_pngRgba16), s_pngRgba16Exp, 3, 2);
    check_png(ctx, s_pngPal, sizeof(s_pngPal), s_pngPalExp, 9, 9);
    // Refused: too many pixels, a cut file, a damaged signature.
    H64_CHECK(ctx, h64_png_decode(s_pngRgb, sizeof(s_pngRgb), &w, &h, 29) == NULL);
    H64_CHECK(ctx, h64_png_decode(s_pngRgb, sizeof(s_pngRgb) - 40, &w, &h, 1u << 20) == NULL);
    bad = (u8 *)malloc(sizeof(s_pngRgb));
    memcpy(bad, s_pngRgb, sizeof(s_pngRgb));
    bad[1] = 'Q';
    H64_CHECK(ctx, h64_png_decode(bad, sizeof(s_pngRgb), &w, &h, 1u << 20) == NULL);
    free(bad);

    // Fitting and box-filter scaling: a 4x2 picture to 2x1 averages 2x2 blocks.
    {
        static const u8 src[4 * 2 * 4] = {
            0, 0, 0, 255, 100, 100, 100, 255, 200, 0, 0, 255, 200, 0, 0, 255,
            100, 100, 100, 255, 200, 200, 200, 255, 0, 0, 200, 255, 0, 0, 200, 255,
        };
        u8 *d = h64_image_scale(src, 4, 2, 2, 1);
        int fw, fh;
        H64_CHECK(ctx, d != NULL);
        if (d)
        {
            H64_CHECK_EQ(ctx, d[0], 100);
            H64_CHECK_EQ(ctx, d[4], 100);
            H64_CHECK_EQ(ctx, d[6], 100);
            H64_CHECK_EQ(ctx, d[7], 255);
            free(d);
        }
        h64_image_fit(512, 357, 300, 200, &fw, &fh);
        H64_CHECK_EQ(ctx, fw, 286);
        H64_CHECK_EQ(ctx, fh, 200);
        h64_image_fit(100, 50, 300, 200, &fw, &fh);
        H64_CHECK_EQ(ctx, fw, 100);
    }
}

// Excerpts of libretro-database's dats and GameDB-N64's JSON (shapes as published).
static const char s_noIntro[] =
    "clrmamepro (\n\tname \"Nintendo - Nintendo 64\"\n)\n\n"
    "game (\n\tname \"Super Mario 64 (USA)\"\n\tregion \"USA\"\n\tserial \"NSME\"\n"
    "\trom ( name \"Super Mario 64 (USA).z64\" size 8388608 crc 3CE60709 md5 20B8 serial \"NSME\" )\n)\n"
    "game (\n\tname \"Super Mario 64 (USA)\"\n\tregion \"USA\"\n\tserial \"NSME\"\n"
    "\trom ( name \"Super Mario 64 (USA).v64\" size 8388608 crc 42C43204 serial \"NSME\" )\n)\n"
    "game (\n\tname \"Super Mario 64 (USA) (LodgeNet)\"\n\tregion \"USA\"\n"
    "\trom ( name \"x.z64\" size 8388608 crc 35C02D9F )\n)\n"
    "game (\n\tname \"Legend of Zelda, The - Ocarina of Time (USA)\"\n\tregion \"USA\"\n\tserial \"CZLE\"\n"
    "\trom ( name \"a.z64\" size 33554432 crc CD16C529 serial \"CZLE\" )\n)\n"
    "game (\n\tname \"Legend of Zelda, The - Ocarina of Time (USA) (Rev 1)\"\n\tregion \"USA\"\n\tserial \"CZLE\"\n"
    "\trom ( name \"b.z64\" size 33554432 crc 00000001 serial \"CZLE\" )\n)\n"
    "game (\n\tname \"Legend of Zelda, The - Ocarina of Time (USA) (Beta)\"\n\tregion \"USA\"\n\tserial \"CZLE\"\n"
    "\trom ( name \"c.z64\" size 33554432 crc 00000002 serial \"CZLE\" )\n)\n"
    "game (\n\tname \"Mario Kart 64 (Japan)\"\n\tregion \"Japan\"\n\tserial \"NKTJ\"\n"
    "\trom ( name \"d.z64\" size 12582912 crc 00000003 serial \"NKTJ\" )\n)\n";
static const char s_developer[] =
    "clrmamepro (\n\tname \"Nintendo - Nintendo 64\"\n)\n\n"
    "game (\n\tcomment \"Super Mario 64 (USA)\"\n\tdeveloper \"Nintendo EAD\"\n\trom ( crc 3CE60709 )\n)\n"
    "game (\n\tcomment \"Legend of Zelda, The: Ocarina of Time (USA)\"\n\tdeveloper \"Nintendo EAD\"\n\trom ( crc 00000001 )\n)\n";
static const char s_users[] =
    "game (\n\tcomment \"Super Mario 64 (USA)\"\n\tusers 1\n\trom ( crc 3CE60709 )\n)\n"
    "game (\n\tcomment \"Super Mario 64 (USA)\"\n\treleaseyear \"1996\"\n\tgenre \"Platform\"\n\trom ( crc 3CE60709 )\n)\n"
    "game (\n\tcomment \"Mario Kart 64 (Japan)\"\n\tusers 4\n\trom ( crc 00000003 )\n)\n";
static const char s_gdbJson[] =
    "{\"NUS-NSME-USA\": {\"crc32\": \"0x7fdff7e7\", \"developer\": \"Nintendo\", \"publisher\": \"Nintendo\", "
    "\"rating\": \"K-A / 3+\", \"region\": \"NTSC-U\", \"release_date\": \"1996-09-26\", \"release_name\": \"x\", "
    "\"serial\": \"NUS-NSME-USA\", \"title\": \"Super Mario 64\", \"list\": [1, {\"a\": \"b\"}], \"n\": null}, "
    "\"NUS-NKTJ-JPN\": {\"title\": \"Mario Kart 64\", \"publisher\": \"Nintendo \\u00c9\", \"release_date\": \"1996-12-14\"}}";
static const char s_wiki[] =
    "{\"type\":\"standard\",\"title\":\"Super Mario 64\",\"description\":\"1996 video game\","
    "\"extract\":\"Super Mario 64 is a 1996 platform game for the Nintendo\\u00a064. It was developed by Nintendo EAD. "
    "It is the first \\u201cSuper Mario\\u201d game in 3D. One more. And a fifth sentence.\"}";

void test_gameinfo(H64TestContext *ctx)
{
    H64GameDb *db = h64_gamedb_new();
    H64GameInfo info, back;
    char covers[4][128], key[16], buf[2048];
    int n = 0;

    H64_CHECK_EQ(ctx, h64_gamedb_add_dat(db, s_noIntro, sizeof(s_noIntro) - 1), 7);
    H64_CHECK_EQ(ctx, h64_gamedb_count(db), 6);   // the .v64 entry shares the .z64 one's name
    H64_CHECK_EQ(ctx, h64_gamedb_add_dat(db, s_developer, sizeof(s_developer) - 1), 2);
    H64_CHECK_EQ(ctx, h64_gamedb_add_dat(db, s_users, sizeof(s_users) - 1), 3);
    H64_CHECK_EQ(ctx, h64_gamedb_add_dat(db, "game ( name", 11), -1);

    // By game code: the plain release, its metadata through the .z64 CRC.
    h64_gameinfo_clear(&info);
    H64_CHECK(ctx, h64_gamedb_lookup(db, "NSME", 0, "whatever", &info, covers, 4, &n));
    H64_CHECK(ctx, !strcmp(info.name, "Super Mario 64 (USA)"));
    H64_CHECK(ctx, !strcmp(info.title, "Super Mario 64"));
    H64_CHECK(ctx, !strcmp(info.developer, "Nintendo EAD"));
    H64_CHECK(ctx, !strcmp(info.genre, "Platform"));
    H64_CHECK(ctx, !strcmp(info.year, "1996"));
    H64_CHECK(ctx, !strcmp(info.players, "1"));
    H64_CHECK(ctx, !strcmp(info.region, "USA"));
    H64_CHECK_EQ(ctx, n, 1);

    // The header's version picks the revision; the beta comes last; the
    // article moves to the front of the title.
    h64_gameinfo_clear(&info);
    H64_CHECK(ctx, h64_gamedb_lookup(db, "CZLE", 1, NULL, &info, covers, 4, &n));
    H64_CHECK(ctx, !strcmp(info.name, "Legend of Zelda, The - Ocarina of Time (USA) (Rev 1)"));
    H64_CHECK(ctx, !strcmp(info.title, "The Legend of Zelda: Ocarina of Time"));
    H64_CHECK_EQ(ctx, n, 3);
    H64_CHECK(ctx, !strcmp(covers[1], "Legend of Zelda, The - Ocarina of Time (USA)"));
    H64_CHECK(ctx, !strcmp(covers[2], "Legend of Zelda, The - Ocarina of Time (USA) (Beta)"));
    h64_gameinfo_clear(&info);
    H64_CHECK(ctx, h64_gamedb_lookup(db, "CZLE", 0, NULL, &info, covers, 4, &n));
    H64_CHECK(ctx, !strcmp(info.name, "Legend of Zelda, The - Ocarina of Time (USA)"));
    H64_CHECK(ctx, !strcmp(info.title, "The Legend of Zelda: Ocarina of Time"));   // from the No-Intro name

    // No code: the file name without its tags (GoodN64 style).
    h64_gameinfo_clear(&info);
    H64_CHECK(ctx, h64_gamedb_lookup(db, "", 0, "Legend of Zelda, The - Ocarina of Time (U) (V1.0) [!]", &info, covers, 4, &n));
    H64_CHECK(ctx, !strcmp(info.name, "Legend of Zelda, The - Ocarina of Time (USA)"));
    h64_gameinfo_clear(&info);
    H64_CHECK(ctx, !h64_gamedb_lookup(db, "ZZZE", 0, "Unknown Homebrew", &info, covers, 4, &n));
    H64_CHECK_EQ(ctx, n, 0);

    // GameDB-N64: title, date, rating over libretro's.
    H64_CHECK_EQ(ctx, h64_gamedb_add_gamedb_json(db, s_gdbJson, sizeof(s_gdbJson) - 1), 2);
    h64_gameinfo_clear(&info);
    H64_CHECK(ctx, h64_gamedb_lookup(db, "NSME", 0, NULL, &info, covers, 4, &n));
    H64_CHECK(ctx, !strcmp(info.date, "1996-09-26"));
    H64_CHECK(ctx, !strcmp(info.rating, "K-A / 3+"));
    H64_CHECK(ctx, !strcmp(info.developer, "Nintendo EAD"));
    h64_gameinfo_clear(&info);
    H64_CHECK(ctx, h64_gamedb_lookup(db, "NKTJ", 0, NULL, &info, covers, 4, &n));
    H64_CHECK(ctx, !strcmp(info.players, "1-4"));
    H64_CHECK(ctx, !strcmp(info.publisher, "Nintendo E"));   // folded to ASCII

    // The cache file round trip.
    info.cover = 0;
    strcpy(info.description, "Line one.");
    h64_gameinfo_format(&info, buf, sizeof(buf));
    H64_CHECK(ctx, h64_gameinfo_parse(&back, buf, (u32)strlen(buf)));
    H64_CHECK(ctx, back.found == 1 && back.cover == 0);
    H64_CHECK(ctx, !strcmp(back.title, info.title) && !strcmp(back.players, "1-4") && !strcmp(back.description, "Line one."));
    H64_CHECK(ctx, !h64_gameinfo_parse(&back, "title=x\n", 8));
    H64_CHECK_EQ(ctx, back.cover, -1);

    // Keys, names, URLs, Wikipedia.
    h64_gameinfo_key("NSME", 0x1234, key);
    H64_CHECK(ctx, !strcmp(key, "NSME"));
    h64_gameinfo_key("N\x01ME", 0xABCDEF01u, key);
    H64_CHECK(ctx, !strcmp(key, "XABCDEF01"));
    h64_gameinfo_thumb_name("Ms. Pac-Man: Maze & More?", buf, sizeof(buf));
    H64_CHECK(ctx, !strcmp(buf, "Ms. Pac-Man_ Maze _ More_"));
    h64_url_encode("Legend of Zelda, The (USA).png", buf, sizeof(buf));
    H64_CHECK(ctx, !strcmp(buf, "Legend%20of%20Zelda%2C%20The%20%28USA%29.png"));
    H64_CHECK(ctx, h64_gameinfo_wiki_extract(s_wiki, sizeof(s_wiki) - 1, buf, 140));
    H64_CHECK(ctx, !strcmp(buf, "Super Mario 64 is a 1996 platform game for the Nintendo 64. It was developed by Nintendo EAD. "
                                "It is the first \"Super Mario\" game in 3D."));
    {
        const char *dis = "{\"type\":\"disambiguation\",\"description\":\"game\",\"extract\":\"A. B.\"}";
        const char *series = "{\"type\":\"standard\",\"description\":\"Video game series\",\"extract\":\"A. B.\"}";
        H64_CHECK(ctx, !h64_gameinfo_wiki_extract(dis, (u32)strlen(dis), buf, 160));
        H64_CHECK(ctx, !h64_gameinfo_wiki_extract(series, (u32)strlen(series), buf, 160));
    }
    strcpy(buf, "Pok\xC3\xA9mon \xE2\x80\x94 1080\xC2\xB0");
    h64_ascii_fold(buf);
    H64_CHECK(ctx, !strcmp(buf, "Pokemon - 1080"));
    H64_CHECK(ctx, !strcmp(h64_gameinfo_region_name('P'), "Europe"));
    h64_gamedb_free(db);
}
