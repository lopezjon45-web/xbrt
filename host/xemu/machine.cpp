#include "host/xemu/machine.h"

#include "core/devices.h"
#include "host/kernel.h"
#include "host/xemu/xbrt_xemu.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstdio>

extern "C" void xbox_KernelInterruptsWake(void);

namespace xbrt {
namespace {

constexpr unsigned kMaxBars = 4;
constexpr unsigned kVoiceWorkers = 2;

struct BarMmio final : MmioDevice {
    unsigned bar = 0;
    uint32_t base = 0;
    uint32_t read(GuestVa addr, unsigned size) override { return xbrt_xemu_read(bar, addr.v - base, size); }
    void write(GuestVa addr, unsigned size, uint32_t value) override { xbrt_xemu_write(bar, addr.v - base, size, value); }
};

struct Line {
    std::string_view device;
    std::atomic<bool> up{false};
};

std::array<BarMmio, kMaxBars> g_bar_mmio;
std::array<Line, kMaxBars> g_lines;
XbrtHost g_host;

bool line_up(std::string_view device)
{
    for (const Line &l : g_lines)
        if (l.device == device) return l.up.load(std::memory_order_acquire);
    return false;
}

void on_irq_level(unsigned line, int level)
{
    bool was = g_lines[line].up.exchange(level != 0, std::memory_order_acq_rel);
    if (level && !was) xbox_KernelInterruptsWake();
}

void on_refuse(XbrtRefusal kind, const char *what)
{
    report_missing({kind == XBRT_REFUSE_DEVICE_REGISTER ? MissingKind::DeviceRegister : MissingKind::Feature, true, what, {},
                    "", GetCurrentThreadId()});
}

void on_log(const char *line) { report_log(line); }

[[noreturn]] void refuse(const std::string &what)
{
    report_missing({MissingKind::Feature, true, what, {}, "", GetCurrentThreadId()});
    TerminateProcess(GetCurrentProcess(), 3);
    for (;;) {}
}

}  // namespace

std::string attach_xemu_devices(Cpu &cpu, GuestMemory &memory)
{
    g_host = {memory.host<uint8_t>({0x80000000}), memory.ram_bytes(), cpu.device_core_mask(), kVoiceWorkers, on_irq_level,
              on_refuse, on_log};
    XbrtBar bars[kMaxBars];
    unsigned count = xbrt_xemu_create(&g_host, bars, kMaxBars);
    std::string line;
    for (unsigned i = 0; i < count; i++) {
        const XbrtBar &b = bars[i];
        const DeviceAperture *aperture = nullptr;
        for (const DeviceAperture &a : kDeviceApertures)
            if (a.name == b.device) aperture = &a;
        char text[128];
        if (!aperture || b.index != 0 || b.size != aperture->size) {
            snprintf(text, sizeof text, "xemu %s BAR%u of 0x%X bytes fits no device aperture", b.device, b.index, b.size);
            refuse(text);
        }
        g_bar_mmio[i].bar = i;
        g_bar_mmio[i].base = aperture->base;
        cpu.map_mmio({{aperture->base}, aperture->size}, g_bar_mmio[i]);
        g_lines[b.irq_line].device = aperture->name;
        snprintf(text, sizeof text, "%s%s 0x%08X (xemu)", line.empty() ? "" : ", ", b.device, aperture->base);
        line += text;
    }
    xbox_KernelInterruptsWake();
    xbrt_xemu_run();
    char cores[48];
    snprintf(cores, sizeof cores, ", device threads on cores 0x%llX", static_cast<unsigned long long>(g_host.device_cores));
    return line + cores;
}

}  // namespace xbrt

extern "C" {

int xbox_ApuIrqPending(void) { return xbrt::line_up("APU"); }
int xbox_Nv2aIrqPending(void) { return xbrt::line_up("NV2A"); }

}  // extern "C"
