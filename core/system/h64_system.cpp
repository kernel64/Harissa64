#include "h64_system.h"

#include <stdlib.h>
#include <string.h>

#include "../common/h64_fenv.h"
#include "../common/h64_mem.h"
#include "../common/h64_log.h"
#include "../hle/h64_hle.h"

int h64_system_init(H64System *sys, const u8 *romFile, u32 romSize, const H64Options *opt)
{
    memset(sys, 0, sizeof(*sys));
    if (opt) sys->options = *opt;
    else { sys->options.hleBoot = 1; sys->options.emux = 1; }
    sys->rdram = (u8 *)h64_big_alloc(H64_RDRAM_SIZE);   // large pages on the Xbox 360
    if (!sys->rdram)
        return -1;
    sys->save = (H64SaveMem *)malloc(sizeof(H64SaveMem));
    if (!sys->save || h64_rom_load(&sys->rom, romFile, romSize))
    {
        free(sys->save);
        h64_big_free(sys->rdram);
        sys->save = 0;
        sys->rdram = 0;
        return -1;
    }
    {
        int pak, type = h64_save_type_for_rom(&sys->rom, &pak);
        h64_save_init(sys->save, type, pak);
        H64_INFO("[save] save type: %s%s", h64_save_type_name(type), pak ? ", Controller Pak" : "");
    }
    h64_fenv_init();
    h64_system_reset(sys);
    return 0;
}

void h64_system_free(H64System *sys)
{
    h64_rom_free(&sys->rom);
    h64_rdp_free(sys);
    h64_jit_free(sys);
    if (sys->hle) h64_hle_free(sys->hle);
    sys->hle = 0;
    h64_big_free(sys->rdram);
    sys->rdram = 0;
    free(sys->save);
    sys->save = 0;
}

void h64_system_reset(H64System *sys)
{
    memset(sys->rdram, 0, H64_RDRAM_SIZE);
    memset(sys->spMem, 0, sizeof(sys->spMem));
    sys->tvType = h64_rom_tv_type(&sys->rom);
    h64_sched_init(&sys->sched);
    h64_cpu_reset(&sys->cpu);
    h64_pif_reset(sys);
    h64_devices_reset(sys);
    h64_rsp_reset(sys);
    h64_rdp_reset(sys);
    h64_jit_reset(sys);
    if (sys->hle) h64_hle_free(sys->hle);
    sys->hle = h64_hle_create(sys);
    h64_cpu_reschedule_compare(sys);
    if (sys->options.hleBoot)
        h64_hle_boot(sys);
    else
        H64_WARN("[boot] low-level boot (running IPL3) is not available yet; use the HLE boot");
}

static u32 fnv_u64(u32 h, u64 v)
{
    int i;
    for (i = 0; i < 8; i++) { h ^= (u32)(v >> (i * 8)) & 0xFF; h *= 16777619u; }
    return h;
}

void h64_system_state_hash(const H64System *sys, u32 *cpuHash, u32 *ramHash)
{
    u32 h = 2166136261u, i;
    for (i = 0; i < 32; i++) h = fnv_u64(h, sys->cpu.gpr[i]);
    for (i = 0; i < 32; i++) h = fnv_u64(h, sys->cpu.fgr[i]);
    for (i = 0; i < 32; i++) if (i != CP0_RANDOM) h = fnv_u64(h, sys->cpu.cop0[i]);
    h = fnv_u64(h, sys->cpu.hi);
    h = fnv_u64(h, sys->cpu.lo);
    h = fnv_u64(h, sys->cpu.fcr31);
    *cpuHash = h;
    if (!ramHash) return;
    h = 2166136261u;
    for (i = 0; i < H64_RDRAM_SIZE; i++) { h ^= sys->rdram[i]; h *= 16777619u; }
    *ramHash = h;
}

void h64_system_step(H64System *sys)
{
    while (sys->sched.next <= sys->cpu.cycles)
    {
        int ev = h64_sched_pop_due(&sys->sched, sys->cpu.cycles);
        if (ev < 0) break;
        h64_device_event(sys, ev);
    }
    h64_cpu_step(sys);
}

void h64_system_run_cycles(H64System *sys, u64 cycles)
{
    if (sys->jit) { h64_jit_run(sys, cycles); return; }
    u64 end = sys->cpu.cycles + cycles;
    sys->stop = 0;
    while (sys->cpu.cycles < end && !sys->stop)
    {
        while (sys->sched.next <= sys->cpu.cycles)
        {
            int ev = h64_sched_pop_due(&sys->sched, sys->cpu.cycles);
            if (ev < 0) break;
            h64_device_event(sys, ev);
        }
        h64_cpu_step(sys);
    }
}
