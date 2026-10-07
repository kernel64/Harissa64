// Harissa64 V2 - cartridge save media (EEPROM, SRAM, FlashRAM) and the
// Controller Pak on port 1.
//
// The core keeps the contents and marks what the game writes; the platform
// loads the files at start-up and writes them back (h64_save_load/h64_save_store)
// into one folder per game, h64_save_folder_name().
#ifndef H64_SAVE_H
#define H64_SAVE_H

#include "../common/h64_types.h"
#include "h64_rom.h"

enum H64SaveType
{
    H64_SAVE_NONE = 0,
    H64_SAVE_EEPROM_4K,
    H64_SAVE_EEPROM_16K,
    H64_SAVE_SRAM,
    H64_SAVE_FLASH,
    H64_SAVE_AUTO        // unknown cartridge: EEPROM 4K, and SRAM or FlashRAM on the first access
};

// Bits of H64SaveMem::dirty: what the game wrote since the last store.
#define H64_SAVE_DIRTY_EEPROM 0x01u
#define H64_SAVE_DIRTY_SRAM   0x02u
#define H64_SAVE_DIRTY_FLASH  0x04u
#define H64_SAVE_DIRTY_PAK    0x08u

#define H64_EEPROM_MAX 0x800u
#define H64_SRAM_SIZE  0x8000u
#define H64_FLASH_SIZE 0x20000u
#define H64_PAK_SIZE   0x8000u

struct H64SaveMem
{
    int type;             // H64SaveType
    int pak;              // a Controller Pak is plugged into controller 1
    int domain2;          // what answers at 0x08000000: H64_SAVE_NONE, _SRAM or _FLASH (_AUTO: not decided yet)
    u32 dirty;            // H64_SAVE_DIRTY_* written since the last store
    u32 used;             // H64_SAVE_DIRTY_* accessed or loaded (the media worth storing)
    u8 eeprom[H64_EEPROM_MAX];
    u8 sram[H64_SRAM_SIZE];
    u8 flash[H64_FLASH_SIZE];
    u8 pakData[H64_PAK_SIZE];
    // FlashRAM controller
    int flashMode;        // FLASH_MODE_* (h64_save.cpp)
    u32 flashOffset;      // byte offset of the page to erase or program
    u64 flashStatus;
    u8 flashPage[128];    // page buffer (filled by a PI DMA in write mode)
};

// Save type from the catalogue (header CRCs) or the homebrew header ("ED" at
// 0x3C); *pak: whether the game uses a Controller Pak.
int h64_save_type_for_rom(const H64Rom *rom, int *pak);
const char *h64_save_type_name(int type);

// Blank media (EEPROM, SRAM, FlashRAM erased to 0xFF; a formatted pak) for `type`.
void h64_save_init(H64SaveMem *s, int type, int pak);

// JoyBus: the EEPROM (channel 4) and the pak commands of controller 1.
// Return the number of response bytes written, or -1 for "no response".
int h64_save_eeprom_command(H64SaveMem *s, const u8 *tx, int txLen, u8 *rx, int rxLen);
int h64_save_pak_command(H64SaveMem *s, const u8 *tx, int txLen, u8 *rx, int rxLen);
u8 h64_save_pak_crc(const u8 *data, int len);   // CRC-8 (polynomial 0x85) of a pak data block

// Domain 2 (0x08000000-0x0FFFFFFF): CPU accesses and PI DMAs. They return 0
// when no save medium answers there (open bus).
int h64_save_read32(H64SaveMem *s, u32 cart, u32 *value);
int h64_save_write32(H64SaveMem *s, u32 cart, u32 value);
int h64_save_dma_to_rdram(H64SaveMem *s, u32 cart, u8 *dst, u32 len);
int h64_save_dma_from_rdram(H64SaveMem *s, u32 cart, const u8 *src, u32 len);

// Files. folder: "<name> <game code>", made of letters, digits, spaces, '-'
// and '_' only (FATX-safe, at most 42 characters). load/store take the folder
// path with its trailing separator and read/write eeprom.bin, sram.bin,
// flash.bin and pak1.bin there; store writes only the media in `used` and
// clears `dirty`. Return the number of files read or written, -1 on a write error.
void h64_save_folder_name(const H64Rom *rom, char *out, int outSize);
int h64_save_load(H64SaveMem *s, const char *dir);
int h64_save_store(H64SaveMem *s, const char *dir);

#endif
