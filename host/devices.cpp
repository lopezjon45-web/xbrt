// The device models, each an MmioDevice on Cpu::map_mmio. The two USB host controllers are the fork's OHCI model;
// the NV2A and the APU arrive with U4 and U5, and until then the kernel bridges' questions about them get the answer
// for a console without them, and a register access is a named Missing.
#include "host/kernel.h"

#include "core/devices.h"

#include <windows.h>
#include <xinput.h>

#include <cstdio>

extern "C" {
void xbox_OhciAttach(void);
uint32_t xbox_OhciRead(uint32_t xbox_va, unsigned size);
void xbox_OhciWrite(uint32_t xbox_va, unsigned size, uint32_t value);
}

namespace {

struct Ohci final : xbrt::MmioDevice {
    uint32_t read(xbrt::GuestVa addr, unsigned size) override { return xbox_OhciRead(addr.v, size); }
    void write(xbrt::GuestVa addr, unsigned size, uint32_t value) override { xbox_OhciWrite(addr.v, size, value); }
};

void register_access(const char *verb, uint32_t va, bool stop)
{
    char what[96];
    snprintf(what, sizeof what, "NV2A register 0x%08X %s: not modelled", va, verb);
    xbrt::report_missing({xbrt::MissingKind::DeviceRegister, stop, what, {}, "", GetCurrentThreadId()});
}

}  // namespace

std::string xbrt::attach_devices(Cpu &cpu)
{
    static Ohci ohci;
    xbox_OhciAttach();
    for (std::string_view name : {"USB0 OHCI", "USB1 OHCI"})
        for (const DeviceAperture &d : kDeviceApertures)
            if (d.name == name) cpu.map_mmio({{d.base}, d.size}, ohci);
    return "devices USB0 and USB1 OHCI, a pad on game port 1 with no input source yet (U6); no NV2A or APU (U4, U5)";
}

extern "C" {

int xbox_frontend_nv2a_attached(void) { return 0; }

// The pads' source is the shell's Windows.Gaming.Input with U6; until then every slot reads as empty.
int xbox_shell_pad_read(unsigned, XINPUT_GAMEPAD *) { return 0; }
void xbox_shell_pad_rumble(unsigned, unsigned short, unsigned short) {}

// No keyboard: the console has none.
int xbox_FramebufferKeyDown(int) { return 0; }

// No APU: its interrupt line is never up.
int xbox_ApuIrqPending(void) { return 0; }

// No push-buffer executor: nothing is queued behind the title, and no NV2A page is modelled.
uint32_t xbox_Nv2aBacklog(void) { return 0; }
int xbox_Nv2aRegIsHooked(uint32_t) { return 0; }

uint32_t xbox_Nv2aRegRead(uint32_t va)
{
    register_access("read", va, true);
    return 0;
}

void xbox_Nv2aRegWrite(uint32_t va, uint32_t)
{
    static bool warned;
    if (!warned) register_access("write", va, false);
    warned = true;
}

// The lifted runtime's host video player and framebuffer window. xbrt has neither: the title plays its own movies
// and U5's presenter shows the scanout AvSetDisplayMode names.
int xbox_VideoIsPlaying(void) { return 0; }
int xbox_VideoPlayFile(const char *) { return 0; }
void xbox_FramebufferWindowSet(uint32_t, uint32_t) {}
void xbox_FramebufferWindowStart(void) {}
void xbox_SetDisplayFramebuffer(uint32_t, uint32_t) {}

// A sample of the lifted runtime's GPU state for its exit log; there is no state to sample.
void xbox_PeekSample(const char *) {}

}  // extern "C"
