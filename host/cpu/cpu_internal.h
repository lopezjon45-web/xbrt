// Private to host/cpu (and tests/host, which checks it): the emitted gates, the ThreadBlock, the process-wide CPU
// state and the calls the four translation units make into each other. Nothing here reaches a public header.
#pragma once

#include "core/plan.h"
#include "host/cpu.h"
#include "host/cpu/guest_layout.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

namespace xbrt::detail {

// ---- emit.cpp: bytes only, no addresses but the ones passed in; decodable on the Mac ----

// A guest site of L >= 6 bytes becomes `FF 1D <far pointer>` and nops (gates.md 4).
inline constexpr uint8_t kPatchLength = 6;
inline constexpr uint16_t kCompatCs = 0x23, kLongCs = 0x33, kFlatData = 0x2B;
inline constexpr uint32_t kFatalExitCode = 0xDEAD;   // a fatal trap ends the host thread with it

std::vector<uint8_t> emit_enter64();
std::vector<uint8_t> emit_exit32(uint32_t ptr_exit64);
std::vector<uint8_t> emit_exit64();
std::vector<uint8_t> emit_full_gate64(uint64_t on_gate);
std::vector<uint8_t> emit_resume64();                 // rcx = the CONTEXT to resume from
std::vector<uint8_t> emit_unwind64(uint32_t exit64);  // rcx = ThreadBlock, rdx = the code call() returns
std::vector<uint8_t> emit_id_stub(uint16_t id, uint32_t ptr_full_gate64);
std::vector<uint8_t> emit_ret_stub();
std::vector<uint8_t> emit_fault_gate();   // ud2: the second-fault guard's test target
std::vector<uint8_t> emit_far_pointer(uint32_t offset, uint16_t selector);
std::vector<uint8_t> emit_patch(uint32_t ptr_variant, uint8_t length);
// Empty when the form has no micro-gate (gates.md 4 lists the kinds; esp as the register has none).
std::vector<uint8_t> emit_micro_gate(SiteKind kind, uint8_t reg, uint32_t disp, uint32_t imm);
bool has_micro_gate(SiteKind kind, uint8_t reg);

// ---- the runtime chunk: one 1 MB RWX allocation laid out as gates.md's table ----

inline constexpr uint32_t kChunkSize = 0x100000;
inline constexpr uint32_t kVariantsOffset = 0x1000, kFarPtrOffset = 0x10000, kIdStubsOffset = 0x20000;
inline constexpr uint32_t kIdStubSize = 16;

struct Gates {
    uint32_t enter64, exit32, exit64, full_gate64, resume64, unwind64, ret_stub, fault_gate;
    uint32_t ptr_exit64, ptr_full_gate64;   // far-pointer entries
};

struct VariantKey {
    SiteKind kind;
    uint8_t reg;
    uint32_t disp, imm;
    auto operator<=>(const VariantKey &) const = default;
};

}  // namespace xbrt::detail

#ifdef _WIN32

#include <windows.h>

namespace xbrt::detail {

static_assert(offsetof(CONTEXT, MxCsr) == ctx::MxCsr && offsetof(CONTEXT, EFlags) == ctx::EFlags);
static_assert(offsetof(CONTEXT, Rax) == ctx::Rax && offsetof(CONTEXT, Rcx) == ctx::Rcx && offsetof(CONTEXT, Rdx) == ctx::Rdx);
static_assert(offsetof(CONTEXT, Rbx) == ctx::Rbx && offsetof(CONTEXT, Rsp) == ctx::Rsp && offsetof(CONTEXT, Rbp) == ctx::Rbp);
static_assert(offsetof(CONTEXT, Rsi) == ctx::Rsi && offsetof(CONTEXT, Rdi) == ctx::Rdi && offsetof(CONTEXT, R15) == ctx::R15);
static_assert(offsetof(CONTEXT, Rip) == ctx::Rip && offsetof(CONTEXT, FltSave) == ctx::FltSave);
static_assert(offsetof(GateFrame, eax) == 0 && offsetof(GateFrame, esp) == 0x1C && offsetof(GateFrame, eip) == 0x20);
static_assert(offsetof(GateFrame, mxcsr) == 0x24 && offsetof(GateFrame, fpcw) == 0x28 && sizeof(GateFrame) <= 0x40);

struct TrapFrame {
    CONTEXT ctx;
    EXCEPTION_RECORD rec;
};

struct ThreadBlock {
    XboxKpcr kpcr;
    uint8_t pad0[kEthreadOffset - sizeof(XboxKpcr)];
    XboxEthread ethread;
    uint8_t pad1[kTlsOffset - kEthreadOffset - sizeof(XboxEthread)];
    uint8_t tls[kTlsCapacity];
    HostPart host;
    uint8_t pad2[kTrapFrameOffset - kHostPartOffset - sizeof(HostPart)];
    TrapFrame trap;
    uint8_t pad3[kThreadBlockSize - kTrapFrameOffset - sizeof(TrapFrame)];
};
static_assert(offsetof(ThreadBlock, ethread) == kEthreadOffset && offsetof(ThreadBlock, tls) == kTlsOffset);
static_assert(offsetof(ThreadBlock, host) == kHostPartOffset && offsetof(ThreadBlock, trap) == kTrapFrameOffset);
static_assert(sizeof(ThreadBlock) == kThreadBlockSize);

struct MmioMap {
    uint32_t base, end;
    MmioDevice *device;
};

struct PortMap {
    uint16_t first, end;
    PortDevice *device;
};

struct CpuState {
    GuestMemory *memory = nullptr;
    HostCallHandler on_host_call = nullptr;
    Cpu::FatalSink on_fatal = nullptr;
    CpuOptions options;
    uint32_t chunk = 0;   // the 1 MB RWX chunk; "the gate region" of traps.md row 0 is [chunk, chunk + kChunkSize)
    Gates gates{};
    LARGE_INTEGER qpf{};

    std::mutex patch_lock;
    std::atomic<uint32_t> patching_site{0};   // one site at a time: the lock serializes the protocol
    std::map<VariantKey, uint32_t> variants;  // -> far-pointer entry
    uint32_t variants_cursor = kVariantsOffset, far_ptr_cursor = kFarPtrOffset;
    std::vector<bool> id_stub_made;
    std::vector<PatchSite> learned;

    std::set<uint32_t> promoted;   // every fs or privileged site rewritten, so a late-landing thread reruns it in place
    std::mutex routes_lock;
    std::vector<MmioMap> mmio;
    std::vector<PortMap> ports;
    std::set<uint32_t> rdtsc_sites;

    std::atomic<uint64_t> gate_calls{0}, guest_calls{0}, traps_fs{0}, traps_mmio{0}, traps_port{0}, traps_rdtsc{0},
        traps_privileged{0}, traps_patch_wait{0}, sites_promoted_eager{0}, sites_promoted_learned{0}, fs_unpromotable{0};
    std::atomic<uint32_t> guest_threads{0}, next_index{0};

    std::mutex threads_lock;
    std::vector<std::unique_ptr<GuestThread>> threads;   // by index; never freed, crash.txt names threads by index
};

extern CpuState *g_cpu;
extern thread_local ThreadBlock *t_block;

inline bool in_gate_region(uint64_t rip) { return rip >= g_cpu->chunk && rip < uint64_t(g_cpu->chunk) + kChunkSize; }

// threads.cpp
extern "C" void xbrt_on_gate(GateFrame *frame, ThreadBlock *tb) noexcept;
[[noreturn]] void unwind_thread(ThreadBlock *tb, uint32_t code);
ThreadBlock *block_of(GuestThread &t);
HANDLE thread_handle(GuestThread &t);   // for the tests: join a spawned thread and read its exit code
uint32_t write_far_pointer(uint32_t offset);   // a new entry in the far-pointer table, under the patch lock
uint64_t guest_tsc_now();

// traps.cpp
LONG CALLBACK cpu_veh(EXCEPTION_POINTERS *e);
extern "C" [[noreturn]] void xbrt_trap_landing(ThreadBlock *tb);
[[noreturn]] void fatal_trap(ThreadBlock *tb, const std::string &what);

// promote.cpp
struct FsForm {
    SiteKind kind;
    uint8_t reg;
    uint32_t disp, imm;
};
FsForm classify_fs(std::span<const uint8_t> bytes);
// Rewrites the site under the cross-modifying-code protocol (gates.md 6). False when the form has no micro-gate or
// the chunk is full; the site then keeps trapping.
bool promote(const PatchSite &site, bool learned);

}  // namespace xbrt::detail

#endif
