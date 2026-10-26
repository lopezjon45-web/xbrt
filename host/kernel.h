// The kernel adapter. Every guest -> host call lands in on_host_call: a kernel export runs the fork's bridge for its
// ordinal (external/xboxrecomp, built with XBOXRECOMP_FRONTEND_DIRECT), an HLE override runs its case here. The same
// file defines the fork's front-end seam (xbox/xbox_frontend.h) and the symbols of the fork's lifted-only files over
// GuestMemory and GuestThread, so nothing else in xbrt names the bridges' register file.
#pragma once

#include "core/symbols.h"
#include "core/types.h"
#include "core/xbe.h"
#include "host/cpu.h"
#include "host/memory.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace xbrt {

// design.md "Visible failure": the kinds a gap can be.
enum class MissingKind : uint8_t { KernelExport, KernelStub, Instruction, DeviceRegister, GpuMethod, Feature, Hang };

std::string_view missing_kind_name(MissingKind k);

struct Missing {
    MissingKind kind;
    bool stop;                  // the session ends here; otherwise reported once and the guest continues
    std::string what;           // "Missing kernel export 0x172 (370)", "stub: KeBugCheck (95)"
    GuestVa where;              // the guest return address of the call; 0 when no guest call is in progress
    std::string where_symbol;   // "XAPILIB QueueUserAPC+0x2A", or the address in hex when nothing names it
    uint32_t host_thread;       // the host thread id of the caller
    std::string thread;         // set by the session on receipt: "t<index>", or "host thread <id>" with no guest identity
};

// The shell and the desktop host implement it. A stop never returns to the guest in a shell: the red line, crash.txt
// and the return link with kind= follow. log() takes the kernel's own lines and the guest's OutputDebugStringA.
struct MissingSink {
    virtual void missing(const Missing &m) noexcept = 0;
    virtual void log(std::string_view line) noexcept = 0;
    // HalReturnToFirmware, XapiBootToDash, XLaunchNewImage of the dashboard: the session ends with result=ok.
    [[noreturn]] virtual void return_to_hub(uint32_t reason) noexcept = 0;

protected:
    ~MissingSink() = default;
};

struct KernelSetup {
    GuestMemory *memory;
    MissingSink *sink;
    const Cpu *cpu;                  // null only where no override runs (the kernel tests)
    std::u16string game_dir;         // D:\ : the extracted disc folder of a dir= link, resolved by the caller
    std::u16string save_dir;         // the title's save partitions and caches
    GuestVa code_lo, code_hi;        // the image's code range, for the kernel's diagnostics
    const SymbolMap *symbols;        // names call sites in a Missing; may be null
    const XbeImage *xbe;             // the image `symbols` was scanned from; may be null
};

// Once per process, before the first guest call: keeps the setup, fills the kernel data page (in the runtime region)
// and maps D:\ and the save folders. A disc image's D:\ needs the XISO volume, which a later unit brings; until then
// an empty game_dir is an error that says so.
Parsed<void> kernel_install(const KernelSetup &setup);

// The HostCallHandler given to Cpu::install.
void on_host_call(GuestThread &thread, HostCallId id, GateFrame &frame) noexcept;

// What a data export's thunk slot holds: its VA in the kernel data page. 0 for a function export.
GuestVa kernel_data_export(Ordinal o);

// For the device answers in host/devices.cpp: reports through the installed sink.
void report_missing(Missing m) noexcept;

}  // namespace xbrt
