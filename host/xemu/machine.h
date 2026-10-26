#pragma once

#include "host/cpu.h"
#include "host/memory.h"

#include <string>

namespace xbrt {

// Creates the devices, maps each BAR on Cpu::map_mmio at its core/devices.h aperture, starts the fork's timer thread
// (it calls the title's ISR while a device's line is up, through xbox_ApuIrqPending and xbox_Nv2aIrqPending) and runs
// the machine. Once per process, after kernel_install and before thread 0 starts. Any failure is a stop refusal.
// Returns the stage line's part for these devices.
std::string attach_xemu_devices(Cpu &cpu, GuestMemory &memory);

// "nv2a methods=.. draws=.. flips=.. vblanks=..; apu frames/s=.. util=..", or empty before the devices are attached.
std::string xemu_counters_line();

}  // namespace xbrt
