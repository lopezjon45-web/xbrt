// U0's CPU-mode probes, in run order; each reports one ProbeLine through the sink (Begin, then Ok or Fail):
//   resume     one hlt in compat mode, resumed through the CB far-return stub (gates.md 5); the guest must continue
//              in compat mode at the next instruction (eax marker), one hit, no second fault
//   selectors  the fs, gs, ds and ss selector values compat-mode code reads (ground-compat open question 2)
//   r15        the ThreadBlock anchor in r15 across 10^5 round trips through code that executes 0x40-0x4F bytes
//   cost       nanoseconds per host->guest->host round trip, per far call to a 64-bit micro-gate and back
//              (gates.md 4), and per VEH trap round trip (hlt resumed through CB), QPC-timed
//   fs         10^6 mov eax,fs:[0] on 4 threads under SwitchToThread churn: every one must fault with a linear
//              address below 0x1000 (the riskiest assumption in design.md); the count that did not is the finding
#pragma once

#include "host/memory.h"

#include <functional>
#include <string>

namespace xbrt {

struct ProbeLine {
    enum class State : uint8_t { Begin, Ok, Fail };
    std::string name;
    State state;
    std::string detail;   // the numbers, for run.txt and the status screen; empty at Begin
};

using ProbeSink = std::function<void(const ProbeLine &)>;

// The run.txt line the shells write for a ProbeLine: "BEGIN probe <name>", "probe <name> ok <detail>" or
// "probe <name> FAIL <detail>". One formatter, so console scripts and the CI judge read the same text everywhere.
inline std::string probe_text(const ProbeLine &p)
{
    switch (p.state) {
    case ProbeLine::State::Begin: return "BEGIN probe " + p.name;
    case ProbeLine::State::Ok: return "probe " + p.name + " ok" + (p.detail.empty() ? "" : " " + p.detail);
    case ProbeLine::State::Fail: return "probe " + p.name + " FAIL" + (p.detail.empty() ? "" : " " + p.detail);
    }
    return "probe " + p.name;
}

// Installs its own VEH for the duration, emits its gates and payloads into the runtime region, runs the probes in
// the order above (Begin, then Ok or Fail, per probe) and removes the VEH. Returns false when any probe failed.
bool run_cpu_probes(GuestMemory &memory, const ProbeSink &sink);

}  // namespace xbrt
