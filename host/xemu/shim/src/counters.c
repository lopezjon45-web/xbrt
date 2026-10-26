#include "shim.h"
#include "hw/xbox/mcpx/apu/apu_debug.h"
#include "hw/xbox/nv2a/nv2a_regs.h"

static uint64_t g_methods, g_draws, g_flips, g_vblanks;

static void add(uint64_t *counter, uint64_t n) { __atomic_fetch_add(counter, n, __ATOMIC_RELAXED); }

void xbrt_trace_nv2a_pgraph_method(uint32_t subchannel, uint32_t graphics_class, uint32_t method, const char *name,
                                   uint32_t offset, uint32_t parameter)
{
    (void)subchannel;
    (void)name;
    (void)offset;
    add(&g_methods, 1);
    if (graphics_class == NV_KELVIN_PRIMITIVE && method == NV097_SET_BEGIN_END && parameter == NV097_SET_BEGIN_END_OP_END)
        add(&g_draws, 1);
}

/* pgraph.c logs a run of ARRAY_ELEMENT16 once, when the run ends, with the run's length less one. */
void xbrt_trace_nv2a_pgraph_method_abbrev(uint32_t subchannel, uint32_t graphics_class, uint32_t method,
                                          const char *name, unsigned int count)
{
    (void)subchannel;
    (void)graphics_class;
    (void)method;
    (void)name;
    add(&g_methods, (uint64_t)count + 1);
}

void xbrt_trace_nv2a_pgraph_flip_stall(void) { add(&g_flips, 1); }

void xbrt_count_vblank(void) { add(&g_vblanks, 1); }

void xbrt_xemu_counters(XbrtCounters *out)
{
    const struct McpxApuDebug *apu = mcpx_apu_get_debug_info();
    *out = (XbrtCounters){
        .nv2a_methods = __atomic_load_n(&g_methods, __ATOMIC_RELAXED),
        .nv2a_draws = __atomic_load_n(&g_draws, __ATOMIC_RELAXED),
        .nv2a_flips = __atomic_load_n(&g_flips, __ATOMIC_RELAXED),
        .vblanks = __atomic_load_n(&g_vblanks, __ATOMIC_RELAXED),
        .apu_frames_per_second = apu->frames_processed,
        .apu_utilization = apu->utilization,
    };
}
