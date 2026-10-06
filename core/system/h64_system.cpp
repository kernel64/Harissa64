#include "h64_system.h"

#include <stdlib.h>
#include <string.h>

#include "../common/h64_fenv.h"
#include "../common/h64_log.h"

int h64_system_init(H64System *sys, const u8 *romFile, u32 romSize, const H64Options *opt)
{
    memset(sys, 0, sizeof(*sys));
    if (opt) sys->options = *opt;
    else { sys->options.hleBoot = 1; sys->options.emux = 1; }
    sys->rdram = (u8 *)malloc(H64_RDRAM_SIZE);
    if (!sys->rdram)
        return -1;
    if (h64_rom_load(&sys->rom, romFile, romSize))
    {
        free(sys->rdram);
        sys->rdram = 0;
        return -1;
    }
    h64_fenv_init();
    h64_system_reset(sys);
    return 0;
}

void h64_system_free(H64System *sys)
{
    h64_rom_free(&sys->rom);
    free(sys->rdram);
    sys->rdram = 0;
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
    h64_cpu_reschedule_compare(sys);
    if (sys->options.hleBoot)
        h64_hle_boot(sys);
    else
        H64_WARN("[boot] low-level boot (running IPL3) is not available yet; use the HLE boot");
}

void h64_system_run_cycles(H64System *sys, u64 cycles)
{
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
        {
            u64 before = sys->cpu.cycles;
            h64_cpu_step(sys);
            h64_rsp_advance(sys, (u32)(sys->cpu.cycles - before));
        }
    }
}
