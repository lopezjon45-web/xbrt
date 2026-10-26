#pragma once

#include "host/memory.h"

#include <functional>
#include <string>

namespace xbrt {

// Calls on_host_call for kernel exports through hand-built GateFrames and checks what comes back. Writes one line
// per test through `line`; true when every test passed. Creates kernel-disc\ (mapped as D:\) and kernel-saves\ in
// the current directory.
bool run_kernel_tests(GuestMemory &memory, const std::function<void(const std::string &)> &line);

}  // namespace xbrt
