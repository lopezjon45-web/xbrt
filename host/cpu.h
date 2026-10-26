// The guest CPU: 32-bit Xbox code running natively in compat mode, and every way control crosses between it and host
// code. This header is the whole public surface; host/cpu/* behind it holds the gate emitter (gates.md), r15 and the
// ThreadBlock, stack switching and nesting, MXCSR/FPCW across crossings, the VEH, the one-instruction emulator, the
// far-return resume, the patch protocol and the counters.
#pragma once

#include "core/plan.h"
#include "core/types.h"
#include "host/memory.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace xbrt {

namespace detail {
struct ThreadBlock;
struct ThreadAccess;
}

// One id space for everything a guest call can reach on the host: kernel exports and HLE overrides. The two makers
// are the only way to build one, and their ranges cannot overlap.
class HostCallId {
public:
    static constexpr HostCallId kernel(Ordinal o) { return HostCallId(o.v); }                  // 1 .. 511
    static constexpr HostCallId override_(HleId h) { return HostCallId(512 + uint16_t(h)); }   // 512 ..
    static constexpr HostCallId from_raw(uint16_t v) { return HostCallId(v); }
    constexpr bool is_kernel() const { return v_ < 512; }
    constexpr Ordinal ordinal() const { return {v_}; }
    constexpr HleId hle() const { return HleId(v_ - 512); }
    constexpr uint16_t raw() const { return v_; }

private:
    constexpr explicit HostCallId(uint16_t v) : v_(v) {}
    uint16_t v_;
};

// The guest's registers at a guest -> host call, on the host stack (one per nesting level; gates.md 3). A handler
// reads arguments through esp, writes results, and must set eip and esp to where the guest continues (stdcall:
// eip = [esp], esp += 4 + arg bytes). Flags are not part of it: they are volatile across an x86 call.
struct GateFrame {
    uint32_t eax, ecx, edx, ebx, ebp, esi, edi;
    uint32_t esp;   // at entry: points at the guest return address
    uint32_t eip;   // at entry: the guest return address ([esp])
    uint32_t mxcsr;
    uint16_t fpcw, pad_;
};

class GuestThread;
using HostCallHandler = void (*)(GuestThread &thread, HostCallId id, GateFrame &frame) noexcept;

// A device whose registers live on trapping pages. Called on the faulting guest thread from the trap router, on the
// host stack (the VEH redirects to a landing first, SYNTHESIS.md graft). Keep it short: post heavy work to the
// device's own thread.
struct MmioDevice {
    virtual uint32_t read(GuestVa addr, unsigned size) = 0;   // size 1, 2, 4
    virtual void write(GuestVa addr, unsigned size, uint32_t value) = 0;

protected:
    ~MmioDevice() = default;
};

struct PortDevice {
    virtual uint32_t in(uint16_t port, unsigned size) = 0;
    virtual void out(uint16_t port, unsigned size, uint32_t value) = 0;

protected:
    ~PortDevice() = default;
};

struct PortRange {
    uint16_t first, count;
};

// The counters line and run.txt every 10 s.
struct CpuCounters {
    uint64_t gate_calls, guest_calls;
    uint64_t traps_fs, traps_mmio, traps_port, traps_rdtsc, traps_privileged, traps_patch_wait;
    uint64_t sites_promoted_eager, sites_promoted_learned, fs_unpromotable;
    uint32_t live_guest_threads;
};

struct CpuOptions {
    bool pin_guest_threads_to_one_core = true;   // the movie streamer of the first title needs it (xboxrecomp)
    double guest_tsc_hz = 733'333'333.0;         // rdtsc and QueryPerformanceCounter scale to this
    bool trace_traps = false;
};

// What a stop-kind trap leaves behind for crash.txt and the red screen (design.md "Visible failure").
struct TrapReport {
    std::string what;          // "cannot emulate 'out dx,eax' (port 0x80C4)", "fault on 0xC0000000 (page tables)"
    bool device_register;      // an access to a device aperture no model serves
    GuestVa where;
    uint32_t thread;
    std::array<uint8_t, 32> bytes_at_eip;
    std::array<uint32_t, 16> stack_at_esp;   // zero where unreadable
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi, eflags;
    std::vector<GuestVa> ebp_chain;   // a guest stack walk, best effort
    // The faulting thread's KPCR and KTHREAD fields that fs: and TLS accesses go through.
    uint32_t exception_list, stack_base, stack_limit, irql, current_thread, tls_data, tls_slot0;
};

class Cpu {
public:
    // Installs the VEH (first in the chain), emits the fixed gates into the runtime region, records the handler that
    // receives every guest -> host call and the sink that receives a fatal trap. Once per process.
    using FatalSink = void (*)(const TrapReport &) noexcept;
    static Cpu &install(GuestMemory &memory, HostCallHandler on_host_call, FatalSink on_fatal, const CpuOptions &options);

    // The 32-bit address a guest call must reach for `id`: an id stub (gates.md 3), made on first request. Session
    // writes these into the XBE thunk slots and behind each override's E9 jump.
    GuestVa gate_for(HostCallId id);

    // Rewrites guest code at each site (gates.md 6) after checking the original bytes. Sites whose bytes differ are
    // skipped and returned, so a stale plan cache never corrupts code.
    std::vector<PatchSite> apply_sites(std::span<const PatchSite> sites);
    // Overrides: E9 rel32 at each entry to gate_for(override_(id)).
    void apply_overrides(std::span<const OverrideSite> overrides);
    // Sites promoted from traps this run, for merge_learned and plan.bin.
    std::vector<PatchSite> learned_sites() const;

    // Device registration. Pages become no-access; a guest load or store on them becomes read() or write().
    void map_mmio(GuestRange range, MmioDevice &device);
    void map_ports(PortRange range, PortDevice &device);
    // Addresses in the Rdtsc set (CodeMap) whose `0F 31` became `EF 90`; the router scales them to guest_tsc_hz.
    void note_rdtsc_sites(std::span<const GuestVa> sites);

    CpuCounters counters() const;
    uint64_t guest_tsc() const;

private:
    Cpu() = default;
};

enum class CallConv : uint8_t { Stdcall, Cdecl, Fastcall, Thiscall };

struct SpawnOptions {
    uint32_t stack_size;      // PsCreateSystemThreadEx's KernelStackSize, or the XBE's stack commit
    int priority;             // Xbox priority mapped to a host priority
    bool suspended;
    uint32_t tls_data_size;   // XbeImage::tls_block_size(): KTHREAD.TlsData = StackBase - this
};

// One per host thread that ever runs guest code. A GuestThread owns a ThreadBlock (Xbox KPCR, KPRCB, ETHREAD and TLS
// at their Xbox offsets, below 4 GB) and a guest stack in system space.
class GuestThread {
public:
    // The calling host thread's guest identity, created on first use (how the runtime's own threads call guest ISRs
    // and callbacks).
    static GuestThread &current();
    static GuestThread *current_if_any();

    // A new host thread whose first act is call(entry, args, Stdcall).
    static GuestThread &spawn(GuestVa entry, std::span<const uint32_t> args, const SpawnOptions &options);

    // Runs guest code at `va` on this thread's guest stack (below the current guest esp when nested inside a host
    // call) and returns edx:eax. Re-entrant: a host call made by that guest code can call guest code again.
    uint64_t call(GuestVa va, std::span<const uint32_t> args, CallConv conv);

    // PsTerminateSystemThread or the thread's routine returning: unwinds every nested host frame of this thread to
    // its base (the bridges are C, so no RAII resource is live between) and ends the host thread with `code`.
    [[noreturn]] void exit(uint32_t code);

    GuestVa kpcr() const;      // fs:[0] of this thread == its ThreadBlock base
    GuestVa ethread() const;   // KPRCB.CurrentThread
    uint32_t index() const;    // 0 for the main thread; the survey and crash.txt name threads by it
    bool has_guest_frame() const;
    void resume();             // NtResumeThread on a suspended spawn
    uint32_t host_thread_id() const;

private:
    friend struct detail::ThreadAccess;
    explicit GuestThread(detail::ThreadBlock *block) : block_(block) {}
    detail::ThreadBlock *block_;
};

}  // namespace xbrt
