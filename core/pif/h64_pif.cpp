// Harissa64 V2 - PIF: JoyBus command processing, the PIF RAM control byte,
// the CIC-NUS-6105 challenge, and the high-level boot (IPL1-IPL3 effects).
//
// Behaviour from n64brew ("PIF-NUS", "Joybus Protocol", "Boot process").
#include "../system/h64_system.h"

#include <string.h>

#include "../common/h64_endian.h"
#include "../common/h64_log.h"

// ---- CIC-NUS-6105 challenge/response ----
// Algorithm by X-Scale, as distributed with mupen64plus (n64_cic_nus_6105.c)
// and Harissa64 V1. See THIRD_PARTY.md. Its licence:
//
// Copyright 2011 X-Scale. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//    1. Redistributions of source code must retain the above copyright notice,
//       this list of conditions and the following disclaimer.
//    2. Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY X-Scale ``AS IS'' AND ANY EXPRESS OR IMPLIED
// WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
// EVENT SHALL X-Scale OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
// INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
// OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
// EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// The views and conclusions contained in the software and documentation are
// those of the authors and should not be interpreted as representing official
// policies, either expressed or implied, of X-Scale.
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
// A standard controller on port 1 (state in sys->pad[0]) with a Controller
// Pak when the game uses one, nothing on ports 2-4, and the cartridge EEPROM
// on channel 4 (h64_save.cpp). Absent devices answer with the "no response" bit.
static void joybus_command(H64System *sys, int channel, const u8 *tx, int txLen, u8 *rx, int rxLen, u8 *rxLenByte)
{
    if (channel == 0 && txLen >= 1)
    {
        switch (tx[0])
        {
        case 0x00:   // info
        case 0xFF:   // reset + info
            // controller; status bit 0: a pak is plugged in (0x02 would mean "pak removed")
            if (rxLen >= 3) { rx[0] = 0x05; rx[1] = 0x00; rx[2] = sys->save->pak ? 0x01 : 0x00; }
            return;
        case 0x02:   // pak read
        case 0x03:   // pak write
            if (h64_save_pak_command(sys->save, tx, txLen, rx, rxLen) >= 0) return;
            break;
        case 0x01:   // buttons and stick
            if (rxLen >= 4)
            {
                if (sys->padHook) sys->padHook(sys);
                rx[0] = (u8)(sys->pad[0].buttons >> 8);
                rx[1] = (u8)sys->pad[0].buttons;
                rx[2] = (u8)sys->pad[0].x;
                rx[3] = (u8)sys->pad[0].y;
                if (sys->pad[0].buttons)
                    H64_DEBUG("[pif] f%u controller 1 read: buttons %04X", sys->vi.frames, sys->pad[0].buttons);
            }
            return;
        }
    }
    if (channel == 4 && h64_save_eeprom_command(sys->save, tx, txLen, rx, rxLen) >= 0) return;
    *rxLenByte |= 0x80;   // no response
}

// Runs the JoyBus command block in PIF RAM. On an SI write (RDRAM -> PIF)
// it runs when the control byte asks for it; on an SI read (PIF -> RDRAM)
// the controller commands run again, because libultra writes the block once
// and then only reads (osContStartReadData), as mupen64plus does it
// (update_pif_read).
static void run_block(H64System *sys, int controllersOnly)
{
    u8 *ram = sys->pifRam;
    int i = 0, channel = 0;

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
            if (!controllersOnly || channel < 4)
                joybus_command(sys, channel, ram + txData, txLen, ram + rxData, rxLen, ram + rxPos);
            i = rxData + rxLen;
            channel++;
        }
    }
}

void h64_pif_run_commands(H64System *sys)
{
    u8 control = sys->pifRam[0x3F];
    if (control & 0x02) { pif_challenge(sys); return; }
    if (!(control & 0x01)) return;
    run_block(sys, 0);
    sys->pifRam[0x3F] &= ~0x01u;
}

void h64_pif_read_hook(H64System *sys)
{
    if (sys->pifRam[0x3F] & 0x02) return;   // challenge pending/in progress
    run_block(sys, 1);
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
    // The 6103 and 6106 IPL3s load to (and jump to) the header's entry point
    // minus 0x100000 / 0x200000 (Banjo-Kazooie and Paper Mario: 0x80100400 ->
    // 0x80000400, 0x80125C00 -> 0x80025C00).
    u32 entry = sys->rom.entryPoint - (sys->rom.cic == H64_CIC_6103 ? 0x100000u :
                                       sys->rom.cic == H64_CIC_6106 ? 0x200000u : 0u);
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

    // Boot variables at 0x80000300 that libultra reads (osTvType, osRomType,
    // osRomBase, osResetType, osCicId, osVersion, osMemSize).
    h64_store_be32(sys->rdram + 0x300, (u32)sys->tvType);
    h64_store_be32(sys->rdram + 0x304, 0);             // ROM type: cartridge
    h64_store_be32(sys->rdram + 0x308, 0xB0000000u);   // osRomBase
    h64_store_be32(sys->rdram + 0x30C, 0);             // reset type: cold
    h64_store_be32(sys->rdram + 0x310, sys->rom.cicSeed);
    h64_store_be32(sys->rdram + 0x314, 0);             // version
    h64_store_be32(sys->rdram + (sys->rom.cic == H64_CIC_6105 ? 0x3F0 : 0x318), H64_RDRAM_SIZE);

    sys->pi.regs[5] = hdr & 0xFF;            // BSD_DOM1_LAT
    sys->pi.regs[6] = (hdr >> 8) & 0xFF;     // BSD_DOM1_PWD
    sys->pi.regs[7] = (hdr >> 16) & 0x0F;    // BSD_DOM1_PGS
    sys->pi.regs[8] = (hdr >> 20) & 0x03;    // BSD_DOM1_RLS
    // DMA registers as IPL3 leaves them (PeterLemon DMAAlignment-PI-ROM-FROM).
    sys->pi.regs[0] = 0x00101000;
    sys->pi.regs[1] = 0x1000000C;
    sys->pi.regs[2] = 0x7F;
    sys->pi.regs[3] = 0x7F;
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
    // Cause as the IPL3 leaves it on hardware (PeterLemon COP0Cause reference
    // capture: BD, CE = 3, ExcCode 31).
    cpu->cop0[CP0_CAUSE] = 0xB000007Cu;
    cpu->cop0[CP0_LLADDR] = 0xFFFFFFFFu;               // usual power-on value (PeterLemon LL_LLD_SC_SCD)
    cpu->pc = (u64)(s64)(s32)entry;
    cpu->nextPc = cpu->pc + 4;
    H64_INFO("[boot] HLE boot: CIC %s seed %02X, TV type %d, entry %08X", h64_cic_name(sys->rom.cic),
             sys->rom.cicSeed, sys->tvType, entry);
}
