#include <windows.h>

#include "shim.h"
#include "qemu/main-loop.h"
#include "xemu-config.h"

void mcpx_apu_init(PCIBus *bus, int devfn, MemoryRegion *ram);

static const XbrtDeviceSpec kDevices[] = {
    {"APU", mcpx_apu_init, PCI_DEVFN(5, 0)},
};

const XbrtHost *g_xbrt_host;
XemuConfig g_config;

void xbrt_refuse(XbrtRefusal kind, const char *fmt, ...)
{
    char what[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(what, sizeof what, fmt, args);
    va_end(args);
    xbrt_bql_release_if_held();
    g_xbrt_host->refuse(kind, what);
    TerminateProcess(GetCurrentProcess(), 3);
    for (;;) {}
}

void xbrt_assert_failed(const char *expr, const char *file, int line)
{
    xbrt_refuse(XBRT_REFUSE_FEATURE, "xemu assert '%s' failed at %s:%d", expr, file, line);
}

void xbrt_shim_exit(int code, const char *file, int line)
{
    xbrt_refuse(XBRT_REFUSE_FEATURE, "xemu device code called exit(%d) at %s:%d", code, file, line);
}

void xbrt_shim_abort(const char *file, int line)
{
    xbrt_refuse(XBRT_REFUSE_FEATURE, "xemu device code called abort() at %s:%d", file, line);
}

unsigned xbrt_machine_create(const XbrtHost *host, const XbrtDeviceSpec *devices, unsigned count, XbrtBar *bars,
                             unsigned max_bars)
{
    static bool created;
    g_xbrt_host = host;
    if (created) xbrt_refuse(XBRT_REFUSE_FEATURE, "xbrt_xemu_create called twice");
    created = true;

    g_config.display.renderer = CONFIG_DISPLAY_RENDERER_NULL;
    g_config.audio.use_dsp = true;
    g_config.audio.use_dsp_jit = false;
    g_config.audio.hrtf = true;
    g_config.audio.volume_limit = 1.0f;
    g_config.audio.vp.num_workers = (int)host->voice_workers;

    xbrt_clock_init();
    xbrt_board_ram.kind = MR_RAM;
    xbrt_board_ram.name = "xbox.ram";
    xbrt_board_ram.size = host->ram_bytes;
    xbrt_board_ram.ram = host->ram;

    bql_lock();
    for (unsigned i = 0; i < count; i++) {
        devices[i].init(NULL, devices[i].devfn, &xbrt_board_ram);
        if (xbrt_pci_device_count() != i + 1)
            xbrt_refuse(XBRT_REFUSE_FEATURE, "%s: its init created %u PCI devices, not one", devices[i].device,
                        xbrt_pci_device_count() - i);
    }
    bql_unlock();
    return xbrt_memory_freeze(devices, bars, max_bars);
}

unsigned xbrt_xemu_create(const XbrtHost *host, XbrtBar *bars, unsigned max_bars)
{
    return xbrt_machine_create(host, kDevices, ARRAY_SIZE(kDevices), bars, max_bars);
}

void xbrt_xemu_run(void)
{
    bql_lock();
    xbrt_qom_reset_all();
    bql_unlock();
    xbrt_clock_start();
    bql_lock();
    xbrt_runstate_start();
    bql_unlock();
}
