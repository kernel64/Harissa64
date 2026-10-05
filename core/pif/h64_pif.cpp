// Harissa64 V2 - PIF: JoyBus command processing, the PIF RAM control byte,
// the CIC-NUS-6105 challenge, and the high-level boot (IPL1-IPL3 effects).
//
// Behaviour from n64brew ("PIF-NUS", "Joybus Protocol", "Boot process").
#include "../system/h64_system.h"

#include <string.h>

#include "../common/h64_endian.h"
#include "../common/h64_log.h"

// ---- CIC-NUS-6105 challenge/response ----
// Algorithm by X-Scale (2011), BSD 2-clause licence, as distributed with
// mupen64plus (n64_cic_nus_6105.c) and Harissa64 V1. See THIRD_PARTY.md.
static void cic_6105_response(const u8 *chl, u8 *rsp, int len)
{
    static const u8 lut0[16] = { 0x4, 0x7, 0xA, 0x7, 0xE, 0x5, 0xE, 0x1, 0xC, 0xF, 0x8, 0xF, 0x6, 0x3, 0x6, 0x9 };
    static const u8 lut1[16] = { 0x4, 0x1, 0xA, 0x7, 0xE, 0x5, 0xE, 0x1, 0xC, 0x9, 0x8, 0x5, 0x6, 0x3, 0xC, 0x9 };
    u8 key = 0xB;
    const u8 *lut = lut0;
    int i;
    for (i = 0; i < len; i++)
    {
        int sgn, mag, mod;
        rsp[i] = (u8)((key + 5 * chl[i]) & 0xF);
        key = lut[rsp[i]];
        sgn = (rsp[i] >> 3) & 1;
        mag = ((sgn == 1) ? ~rsp[i] : rsp[i]) & 7;
        mod = (mag % 3 == 1) ? sgn : 1 - sgn;
        if (lut == lut1 && (rsp[i] == 0x1 || rsp[i] == 0x9)) mod = 1;
        if (lut == lut1 && (rsp[i] == 0xB || rsp[i] == 0xE)) mod = 0;
        lut = (mod == 1) ? lut1 : lut0;
    }
}

static void pif_challenge(H64System *sys)
{
    u8 chl[30], rsp[30];
    int i;
    for (i = 0; i < 15; i++)
    {
        chl[i * 2] = (sys->pifRam[0x30 + i] >> 4) & 0xF;
        chl[i * 2 + 1] = sys->pifRam[0x30 + i] & 0xF;
    }
    cic_6105_response(chl, rsp, 30);
    sys->pifRam[0x2E] = 0;   // Banjo-Tooie checks these (mupen64plus-core 5d1dac61)
    sys->pifRam[0x2F] = 0;
    for (i = 0; i < 15; i++)
        sys->pifRam[0x30 + i] = (u8)((rsp[i * 2] << 4) | rsp[i * 2 + 1]);
    sys->pifRam[0x3F] = 0;
}

// ---- JoyBus devices ----
// M1: a standard controller with nothing plugged in on port 1, nothing on
// ports 2-4 and no EEPROM. Absent devices answer with the "no response" bit.
static void joybus_command(H64System *sys, int channel, const u8 *tx, int txLen, u8 *rx, int rxLen, u8 *rxLenByte)
{
    (void)sys;
    if (channel == 0 && txLen >= 1)
    {
        switch (tx[0])
        {
        case 0x00:   // info
        case 0xFF:   // reset + info
            if (rxLen >= 3) { rx[0] = 0x05; rx[1] = 0x00; rx[2] = 0x02; }   // controller, no pak
            return;
        case 0x01:   // buttons and stick
            if (rxLen >= 4) memset(rx, 0, 4);
            return;
        }
    }
    *rxLenByte |= 0x80;   // no response
}

void h64_pif_run_commands(H64System *sys)
{
    u8 *ram = sys->pifRam;
    int i = 0, channel = 0;
    u8 control = ram[0x3F];

    if (control & 0x02) { pif_challenge(sys); return; }
    if (!(control & 0x01)) return;

    while (i < 0x3F)
    {
        u8 tx = ram[i];
        if (tx == 0xFE) break;                  // end of commands
        if (tx == 0xFF || tx == 0xFD) { i++; continue; }   // padding
        if (tx == 0x00) { channel++; i++; continue; }      // skip channel
        {
            int txLen = tx & 0x3F;
            int rxPos = i + 1;
            int rxLen, txData, rxData;
            if (rxPos >= 0x3F) break;
            if (ram[rxPos] == 0xFE) break;
            rxLen = ram[rxPos] & 0x3F;
            txData = rxPos + 1;
            rxData = txData + txLen;
            if (rxData + rxLen > 0x3F) break;
            joybus_command(sys, channel, ram + txData, txLen, ram + rxData, rxLen, ram + rxPos);
            i = rxData + rxLen;
            channel++;
        }
    }
    ram[0x3F] &= ~0x01u;
}

// CPU writes to PIF RAM: the control byte (0x3F) acts immediately.
void h64_pif_write_byte_hook(H64System *sys, u32 offset)
{
    u8 c;
    if (offset != 0x3C)   // the word holding byte 0x3F
        return;
    c = sys->pifRam[0x3F];
    if (c & 0x40) { memset(sys->pifRam, 0, sizeof(sys->pifRam)); return; }   // clear PIF RAM
    if (c & 0x20) sys->pifRam[0x3F] |= 0x80;                               // checksum "acquired"
    if (c & 0x08) sys->pifRam[0x3F] &= ~0x08u;                             // boot terminated
    if (c & 0x02) pif_challenge(sys);
}

void h64_pif_reset(H64System *sys)
{
    memset(sys->pifRam, 0, sizeof(sys->pifRam));
}

// ---- High-level boot ----
// Sets the machine to the state the game's entry point sees after IPL1-3
// have run, without executing them (the PIF boot ROM is not available, and
// IPL3's RDRAM initialisation is not emulated yet):
//  - 1 MB of ROM from 0x1000 copied to the entry point;
//  - the registers IPL3 hands over: s3 ROM type (0 = cartridge), s4 TV type,
//    s5 reset type (0 = cold), s6 CIC seed, s7 version, sp near the top of RDRAM;
//  - osMemSize (0x80000318, or 0x800003F0 for 6105 games) = 8 MB;
//  - PI domain 1 timing from the ROM header, RI as IPL3 programs it;
//  - DMEM holds ROM[0..0x1000) as IPL2 left it.
void h64_hle_boot(H64System *sys)
{
    H64Cpu *cpu = &sys->cpu;
    u32 entry = sys->rom.entryPoint;
    u32 phys = entry & 0x1FFFFFFFu;
    u32 i, len = 0x100000;
    u32 hdr = h64_load_be32(sys->rom.data);

    memcpy(sys->spMem, sys->rom.data, 0x1000);
    for (i = 0; i < len; i++)
    {
        u32 src = 0x1000 + i;
        if (phys + i >= H64_RDRAM_SIZE) break;
        sys->rdram[phys + i] = src < sys->rom.size ? sys->rom.data[src] : 0;
    }

    h64_store_be32(sys->rdram + (sys->rom.cic == H64_CIC_6105 ? 0x3F0 : 0x318), H64_RDRAM_SIZE);

    sys->pi.regs[5] = hdr & 0xFF;            // BSD_DOM1_LAT
    sys->pi.regs[6] = (hdr >> 8) & 0xFF;     // BSD_DOM1_PWD
    sys->pi.regs[7] = (hdr >> 16) & 0x0F;    // BSD_DOM1_PGS
    sys->pi.regs[8] = (hdr >> 20) & 0x03;    // BSD_DOM1_RLS
    sys->ri.regs[0] = 0x0E;                  // RI_MODE
    sys->ri.regs[1] = 0x40;                  // RI_CONFIG
    sys->ri.regs[3] = 0x14;                  // RI_SELECT
    sys->ri.regs[4] = 0x00063634;            // RI_REFRESH

    cpu->gpr[19] = 0;                                    // s3: ROM type (cartridge)
    cpu->gpr[20] = (u64)sys->tvType;                     // s4: TV type
    cpu->gpr[21] = 0;                                    // s5: reset type (cold)
    cpu->gpr[22] = sys->rom.cicSeed;                     // s6: CIC seed
    cpu->gpr[23] = 0;                                    // s7: version
    cpu->gpr[29] = (u64)(s64)(s32)0xA4001FF0u;           // sp
    cpu->cop0[CP0_STATUS] = 0x34000000u;                 // CU1 | CU0 | FR
    cpu->pc = (u64)(s64)(s32)entry;
    cpu->nextPc = cpu->pc + 4;
    H64_INFO("[boot] HLE boot: CIC %s seed %02X, TV type %d, entry %08X", h64_cic_name(sys->rom.cic),
             sys->rom.cicSeed, sys->tvType, entry);
}
