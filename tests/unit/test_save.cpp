// Cartridge save media and the Controller Pak (core/cart/h64_save.cpp).
#include <stdlib.h>
#include <string.h>

#include "../../core/cart/h64_save.h"
#include "../../core/common/h64_endian.h"
#include "../../core/common/h64_test.h"

static void test_save_lookup(H64TestContext *ctx)
{
    H64Rom rom;
    u8 hdr[0x40];
    int pak = -1;
    memset(&rom, 0, sizeof(rom));
    memset(hdr, 0, sizeof(hdr));
    rom.data = hdr;
    rom.size = sizeof(hdr);
    rom.crc1 = 0x3E5055B6u;   // Mario Kart 64 (U)
    rom.crc2 = 0x2E92DA52u;
    H64_CHECK_EQ(ctx, h64_save_type_for_rom(&rom, &pak), H64_SAVE_EEPROM_4K);
    H64_CHECK_EQ(ctx, pak, 1);
    rom.crc1 = 0x12345678u;
    H64_CHECK_EQ(ctx, h64_save_type_for_rom(&rom, &pak), H64_SAVE_AUTO);
    hdr[0x3C] = 'E'; hdr[0x3D] = 'D'; hdr[0x3F] = 0x50;   // homebrew header: FlashRAM
    H64_CHECK_EQ(ctx, h64_save_type_for_rom(&rom, &pak), H64_SAVE_FLASH);
}

static void test_save_folder(H64TestContext *ctx)
{
    H64Rom rom;
    char out[64];
    memset(&rom, 0, sizeof(rom));
    strcpy(rom.name, "THE LEGEND OF ZELDA");
    memcpy(rom.gameCode, "CZLE", 4);
    h64_save_folder_name(&rom, out, sizeof(out));
    H64_CHECK(ctx, !strcmp(out, "THE LEGEND OF ZELDA CZLE"));
    strcpy(rom.name, "Mario-Kart: 64!");
    memset(rom.gameCode, 0, sizeof(rom.gameCode));
    h64_save_folder_name(&rom, out, sizeof(out));
    H64_CHECK(ctx, !strcmp(out, "Mario-Kart 64"));
    rom.name[0] = 0;
    rom.crc1 = 0xABCD0123u;
    h64_save_folder_name(&rom, out, sizeof(out));
    H64_CHECK(ctx, !strcmp(out, "ABCD0123"));
}

static void test_save_eeprom(H64TestContext *ctx, H64SaveMem *s)
{
    u8 tx[10] = { 0x05, 3, 1, 2, 3, 4, 5, 6, 7, 8 }, rx[8];
    h64_save_init(s, H64_SAVE_EEPROM_16K, 0);
    tx[0] = 0x00;
    H64_CHECK_EQ(ctx, h64_save_eeprom_command(s, tx, 1, rx, 3), 3);
    H64_CHECK_EQ(ctx, rx[1], 0xC0);
    tx[0] = 0x05;
    H64_CHECK_EQ(ctx, h64_save_eeprom_command(s, tx, 10, rx, 1), 1);
    H64_CHECK(ctx, (s->dirty & H64_SAVE_DIRTY_EEPROM) != 0);
    tx[0] = 0x04;
    tx[1] = 3;
    H64_CHECK_EQ(ctx, h64_save_eeprom_command(s, tx, 2, rx, 8), 8);
    H64_CHECK(ctx, rx[0] == 1 && rx[7] == 8);
    h64_save_init(s, H64_SAVE_SRAM, 0);
    H64_CHECK_EQ(ctx, h64_save_eeprom_command(s, tx, 2, rx, 8), -1);   // no EEPROM on an SRAM cartridge
}

static void test_save_pak(H64TestContext *ctx, H64SaveMem *s)
{
    u8 data[32], tx[35], rx[33];
    int i;
    for (i = 0; i < 32; i++) data[i] = 0x80;
    H64_CHECK_EQ(ctx, h64_save_pak_crc(data, 32), 0xB8);   // libultra's rumble-pak probe block
    for (i = 0; i < 32; i++) data[i] = (u8)i;
    H64_CHECK_EQ(ctx, h64_save_pak_crc(data, 32), 0x33);
    h64_save_init(s, H64_SAVE_EEPROM_4K, 1);
    // Formatted pak: ID block checksums and the index table checksum.
    H64_CHECK_EQ(ctx, h64_load_be16(s->pakData + 0x3C), 0x6625);
    H64_CHECK_EQ(ctx, h64_load_be16(s->pakData + 0x3E), 0x99CD);
    H64_CHECK_EQ(ctx, s->pakData[0x101], 0x71);
    H64_CHECK_EQ(ctx, s->pakData[0x201], 0x71);
    // Write at 0x0300 (address CRC bits ignored), read it back.
    tx[0] = 0x03; tx[1] = 0x03; tx[2] = 0x1F;
    memcpy(tx + 3, data, 32);
    H64_CHECK_EQ(ctx, h64_save_pak_command(s, tx, 35, rx, 1), 1);
    H64_CHECK_EQ(ctx, rx[0], 0x33);
    tx[0] = 0x02;
    H64_CHECK_EQ(ctx, h64_save_pak_command(s, tx, 3, rx, 33), 33);
    H64_CHECK(ctx, !memcmp(rx, data, 32) && rx[32] == 0x33);
    tx[1] = 0x80; tx[2] = 0x01;   // accessory area: reads zeros
    h64_save_pak_command(s, tx, 3, rx, 33);
    H64_CHECK(ctx, rx[0] == 0 && rx[31] == 0 && rx[32] == 0);
}

static void test_save_flash(H64TestContext *ctx, H64SaveMem *s)
{
    u8 page[128], out[128];
    int i;
    for (i = 0; i < 128; i++) page[i] = (u8)(i * 3);
    h64_save_init(s, H64_SAVE_AUTO, 0);
    // Status first: the unknown cartridge becomes FlashRAM.
    h64_save_write32(s, 0x08010000u, 0xE1000000u);
    H64_CHECK_EQ(ctx, s->domain2, H64_SAVE_FLASH);
    h64_save_dma_to_rdram(s, 0x08000000u, out, 8);
    H64_CHECK_EQ(ctx, h64_load_be32(out), 0x11118001u);
    H64_CHECK_EQ(ctx, h64_load_be32(out + 4), 0x00C2001Eu);
    // Program page 5.
    h64_save_write32(s, 0x08010000u, 0xB4000000u);
    h64_save_dma_from_rdram(s, 0x08000000u, page, 128);
    h64_save_write32(s, 0x08010000u, 0xA5000005u);
    h64_save_write32(s, 0x08010000u, 0xD2000000u);
    H64_CHECK(ctx, !memcmp(s->flash + 5 * 128, page, 128));
    // Read it back: the bus address is in halfwords.
    h64_save_write32(s, 0x08010000u, 0xF0000000u);
    h64_save_dma_to_rdram(s, 0x08000000u + 5 * 64, out, 128);
    H64_CHECK(ctx, !memcmp(out, page, 128));
    // Erase it.
    h64_save_write32(s, 0x08010000u, 0x4B000005u);
    h64_save_write32(s, 0x08010000u, 0x78000000u);
    h64_save_write32(s, 0x08010000u, 0xD2000000u);
    H64_CHECK(ctx, s->flash[5 * 128] == 0xFF && s->flash[5 * 128 + 127] == 0xFF);
}

static void test_save_sram(H64TestContext *ctx, H64SaveMem *s)
{
    u8 buf[16], out[16];
    u32 v = 0;
    int i;
    for (i = 0; i < 16; i++) buf[i] = (u8)(0xA0 + i);
    h64_save_init(s, H64_SAVE_AUTO, 0);
    H64_CHECK_EQ(ctx, h64_save_dma_from_rdram(s, 0x08000100u, buf, 16), 1);
    H64_CHECK_EQ(ctx, s->domain2, H64_SAVE_SRAM);
    H64_CHECK(ctx, (s->dirty & H64_SAVE_DIRTY_SRAM) != 0);
    h64_save_dma_to_rdram(s, 0x08000100u, out, 16);
    H64_CHECK(ctx, !memcmp(out, buf, 16));
    h64_save_read32(s, 0x08000104u, &v);
    H64_CHECK_EQ(ctx, v, 0xA4A5A6A7u);
    h64_save_init(s, H64_SAVE_EEPROM_4K, 0);
    H64_CHECK_EQ(ctx, h64_save_read32(s, 0x08000000u, &v), 0);   // open bus
}

void test_save(H64TestContext *ctx)
{
    H64SaveMem *s = (H64SaveMem *)malloc(sizeof(H64SaveMem));
    H64_CHECK(ctx, s != 0);
    if (!s) return;
    test_save_lookup(ctx);
    test_save_folder(ctx);
    test_save_eeprom(ctx, s);
    test_save_pak(ctx, s);
    test_save_flash(ctx, s);
    test_save_sram(ctx, s);
    free(s);
}
