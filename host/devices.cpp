// The device models. None is registered yet: the APU, OHCI and NV2A arrive with U4 and U5, each as an MmioDevice on
// Cpu::map_mmio. Until then the kernel bridges' questions about devices get the answer for a console with none of
// them, and a register access is a named Missing.
#include "host/kernel.h"

#include <windows.h>

#include <cstdio>

namespace {

void register_access(const char *verb, uint32_t va, bool stop)
{
    char what[96];
    snprintf(what, sizeof what, "NV2A register 0x%08X %s: not modelled", va, verb);
    xbrt::report_missing({xbrt::MissingKind::DeviceRegister, stop, what, {}, "", GetCurrentThreadId()});
}

}  // namespace

extern "C" {

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
