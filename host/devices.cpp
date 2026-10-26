// The device models, each an MmioDevice on Cpu::map_mmio. The two USB host controllers are the fork's OHCI model, its
// pad fed by the fork's input reader from the shell's xbox_shell_pad_read; the NV2A and the APU arrive with U4 and U5,
// and until then the kernel bridges' questions about them get the answer for a console without them, and a register
// access is a named Missing.
#include "host/kernel.h"

#include "core/devices.h"

#include <windows.h>

#include <cstdio>

extern "C" {
#include "input/input_host.h"

void xbox_OhciAttach(void);
uint32_t xbox_OhciRead(uint32_t xbox_va, unsigned size);
void xbox_OhciWrite(uint32_t xbox_va, unsigned size, uint32_t value);
uint32_t usb_gamepad_served(int pad, uint8_t last[20]);
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
    InputHostConfig input;
    xbox_HostInputDefaults(&input);
    input.keyboard = INPUT_HOST_KEYBOARD_OFF;   // no keyboard player: it would read as a pad in its slot
    input.players = 1;
    int pads = xbox_HostInputStart(&input);
    xbox_OhciAttach();
    for (std::string_view name : {"USB0 OHCI", "USB1 OHCI"})
        for (const DeviceAperture &d : kDeviceApertures)
            if (d.name == name) cpu.map_mmio({{d.base}, d.size}, ohci);
    return "devices USB0 and USB1 OHCI, the pad on game port 1 read from " + std::to_string(pads) +
           " host pad" + (pads == 1 ? "" : "s") + "; no NV2A or APU (U4, U5)";
}

std::string xbrt::pad_line()
{
    uint8_t r[20];
    uint32_t served = usb_gamepad_served(0, r);
    if (!served) return "pad 1: the title has not read it yet";
    static constexpr struct {
        int byte;
        uint8_t mask;
        const char *name;
    } kDigital[] = {{2, 0x01, "up"}, {2, 0x02, "down"}, {2, 0x04, "left"}, {2, 0x08, "right"},
                    {2, 0x10, "start"}, {2, 0x20, "back"}, {2, 0x40, "ls"}, {2, 0x80, "rs"}};
    static constexpr const char *kAnalog[] = {"A", "B", "X", "Y", "black", "white", "lt", "rt"};
    std::string held;
    for (const auto &d : kDigital)
        if (r[d.byte] & d.mask) held += std::string(" ") + d.name;
    for (int i = 0; i < 8; i++)
        if (r[4 + i] >= 0x40) held += std::string(" ") + kAnalog[i];
    int16_t lx = int16_t(r[12] | r[13] << 8), ly = int16_t(r[14] | r[15] << 8);
    char sticks[48];
    snprintf(sticks, sizeof sticks, ", left stick %d %d", lx, ly);
    return "pad 1: " + std::to_string(served) + " reports to the title, holding" + (held.empty() ? " nothing" : held) +
           sticks;
}

extern "C" {

int xbox_frontend_nv2a_attached(void) { return 0; }

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
