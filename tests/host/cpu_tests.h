// The CPU core's tests, run by the desktop host with --cpu-tests on the windows-2022 runner (no XBE: every payload is
// emitted here). Each test reports one line, "cpu <name> ok <detail>" or "cpu <name> FAIL <detail>"; the judge
// (scripts/check-cpu-tests.py) requires every ok.
#pragma once

#include "host/memory.h"

#include <functional>
#include <string>

namespace xbrt {

bool run_cpu_tests(GuestMemory &memory, const std::function<void(const std::string &)> &sink);

}  // namespace xbrt
