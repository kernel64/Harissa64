// RetroAchievements: rcheevos must read memory the same way on every host.
// Its values are little-endian whatever the host is; on the big-endian Xbox 360
// every 8-bit read gave 0 (rc_read_memory, fixed in third_party/rcheevos).
#include <string.h>

#include "../../core/common/h64_test.h"
#include "../../core/common/h64_types.h"

extern "C" {
#include "../../third_party/rcheevos/include/rc_runtime.h"
}

static u8 s_raMem[8];
static u32 s_raDone;   // bit n: achievement n triggered

static uint32_t RC_CCONV ra_test_read(uint32_t address, uint8_t *buffer, uint32_t count, void *)
{
    if (address >= sizeof(s_raMem) || count > sizeof(s_raMem) - address) return 0;
    memcpy(buffer, s_raMem + address, count);
    return count;
}

static void RC_CCONV ra_test_event(const rc_runtime_event_t *e)
{
    if (e->type == RC_RUNTIME_EVENT_ACHIEVEMENT_TRIGGERED && e->id < 32) s_raDone |= 1u << e->id;
}

void test_ra(H64TestContext *ctx)
{
    // 8, 16, 24 and 32 bits at address 0, a byte at 3, 16 bits big-endian, bit 1 of byte 0.
    static const char *const defs[] = { "0xH0=18", "0x 0=13330", "0xW0=5649426", "0xX0=2018915346",
                                        "0xH3=120", "0xI0=4660", "0xN0=1" };
    static const u8 bytes[4] = { 0x12, 0x34, 0x56, 0x78 };
    rc_runtime_t rt;
    u32 i, n = (u32)(sizeof(defs) / sizeof(defs[0]));
    rc_runtime_init(&rt);
    for (i = 0; i < n; i++) H64_CHECK_EQ(ctx, rc_runtime_activate_achievement(&rt, i + 1, defs[i], NULL, 0), 0);
    // An achievement must be false once before it can trigger.
    memset(s_raMem, 0, sizeof(s_raMem));
    s_raDone = 0;
    rc_runtime_do_frame(&rt, ra_test_event, ra_test_read, NULL, NULL);
    H64_CHECK_EQ(ctx, s_raDone, 0u);
    memcpy(s_raMem, bytes, 4);
    rc_runtime_do_frame(&rt, ra_test_event, ra_test_read, NULL, NULL);
    H64_CHECK_EQ(ctx, s_raDone, ((1u << n) - 1) << 1);
    rc_runtime_destroy(&rt);
}
