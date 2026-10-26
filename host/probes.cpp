#include "host/probes.h"

#include <windows.h>

#include <intrin.h>

#include <atomic>
#include <cstddef>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>

namespace xbrt {

namespace {

constexpr uint16_t kCompatCs = 0x23;
constexpr uint16_t kLongCs = 0x33;
constexpr uint32_t kTrips = 100000;
constexpr uint32_t kCostOps = 100000;
constexpr uint32_t kFsThreads = 4;
constexpr uint32_t kFsPerThread = 250000;
constexpr uint32_t kStackSize = 0x10000;

struct ThreadBlock {
    uint64_t host_rsp;
    uint64_t saved_r14;
    uint64_t saved_r15;
};

using Enter = uint32_t (*)(ThreadBlock *block, uint32_t esp, uint32_t eip);

std::string format(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return buf;
}

struct Emitter {
    std::vector<uint8_t> bytes;
    Emitter &raw(std::initializer_list<uint8_t> l)
    {
        bytes.insert(bytes.end(), l);
        return *this;
    }
    Emitter &u32(uint32_t v)
    {
        for (int i = 0; i < 4; i++) bytes.push_back(uint8_t(v >> (8 * i)));
        return *this;
    }
    Emitter &push_nonvolatiles() { return raw({0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57}); }
    Emitter &pop_nonvolatiles() { return raw({0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C, 0x5F, 0x5E, 0x5D, 0x5B}); }
    Emitter &mov_r15_rcx() { return raw({0x49, 0x89, 0xCF}); }
    Emitter &mov_r14_rcx() { return raw({0x49, 0x89, 0xCE}); }
    Emitter &store_rsp_at_r15() { return raw({0x49, 0x89, 0x27}); }
    Emitter &load_rsp_at_r15() { return raw({0x49, 0x8B, 0x27}); }
    Emitter &store_r14_at_r15(uint8_t disp) { return raw({0x4D, 0x89, 0x77, disp}); }
    Emitter &store_r15_at_r15(uint8_t disp) { return raw({0x4D, 0x89, 0x7F, disp}); }
    Emitter &zero_extend_r8d() { return raw({0x45, 0x89, 0xC0}); }
    Emitter &xor_eax_eax() { return raw({0x31, 0xC0}); }
    Emitter &mov_esp_edx() { return raw({0x89, 0xD4}); }
    Emitter &mov_esp_esp() { return raw({0x89, 0xE4}); }
    Emitter &push_imm8(uint8_t v) { return raw({0x6A, v}); }
    Emitter &push_imm32(uint32_t v) { return raw({0x68}).u32(v); }
    Emitter &push_r8() { return raw({0x41, 0x50}); }
    Emitter &retfq() { return raw({0x48, 0xCB}); }
    Emitter &retf() { return raw({0xCB}); }
    Emitter &ret() { return raw({0xC3}); }
    Emitter &hlt() { return raw({0xF4}); }
    Emitter &mov_eax_imm32(uint32_t v) { return raw({0xB8}).u32(v); }
    Emitter &mov_ecx_imm32(uint32_t v) { return raw({0xB9}).u32(v); }
    Emitter &mov_eax_sreg(uint8_t modrm) { return raw({0x8C, modrm}); }
    Emitter &mov_eax_fs0() { return raw({0x64, 0xA1, 0x00, 0x00, 0x00, 0x00}); }
    Emitter &inc_eax_dec_eax_push_eax_pop_eax() { return raw({0x40, 0x48, 0x50, 0x58}); }
    Emitter &call_far_ptr(uint32_t ptr) { return raw({0xFF, 0x1D}).u32(ptr); }
    Emitter &dec_ecx() { return raw({0x49}); }
    Emitter &jnz_rel8(int8_t rel) { return raw({0x75, uint8_t(rel)}); }
    Emitter &far_pointer(uint32_t offset, uint16_t selector) { return u32(offset).raw({uint8_t(selector), uint8_t(selector >> 8), 0, 0}); }
    Emitter &exit32(uint32_t exit64) { return push_imm8(kLongCs).push_imm32(exit64).retf(); }
};

struct CodeSpan {
    GuestMemory *memory = nullptr;
    uint32_t base = 0, end = 0, cursor = 0;

    uint32_t place(const Emitter &e)
    {
        uint32_t at = (cursor + 15) & ~15u;
        if (at + e.bytes.size() > end) return 0;
        memcpy(memory->host<uint8_t>({at}), e.bytes.data(), e.bytes.size());
        cursor = at + uint32_t(e.bytes.size());
        return at;
    }
};

enum class Access : uint64_t { Read = 0, Write = 1, Fetch = 8 };

struct Fault {
    uint32_t code;
    uint16_t cs;
    uint64_t rip;
    uint64_t addr;
    Access kind;
    uint64_t rsp, rax;
    uint8_t bytes[8];
};

struct VehState {
    uint32_t code_base = 0, code_end = 0;
    uint32_t resume_cb = 0, exit64 = 0;
    std::atomic<uint32_t> traps{0};
    std::atomic<uint32_t> faults{0};
    std::atomic<uint32_t> bad{0};
    std::atomic<uint32_t> stray{0};
    std::atomic<uint64_t> first_bad_addr{0};
    std::atomic<uint64_t> first_bad_rip{0};
    Fault first_stray{};

    void reset()
    {
        traps = 0;
        faults = 0;
        bad = 0;
        stray = 0;
        first_bad_addr = 0;
        first_bad_rip = 0;
        first_stray = {};
    }
};

VehState g_veh;

// Runs on the guest stack in 64-bit mode (the kernel builds the trap frame where the fault happened), so it stays
// short.
LONG CALLBACK veh(EXCEPTION_POINTERS *e)
{
    CONTEXT *c = e->ContextRecord;
    if (c->Rip < g_veh.code_base || c->Rip >= g_veh.code_end) return EXCEPTION_CONTINUE_SEARCH;
    _mm_setcsr(0x1F80);
    __writeeflags(__readeflags() & ~0x400ull);
    const EXCEPTION_RECORD *r = e->ExceptionRecord;
    const uint8_t *insn = reinterpret_cast<const uint8_t *>(static_cast<uintptr_t>(c->Rip));
    uint32_t next;
    if (c->SegCs == kCompatCs && r->ExceptionCode == STATUS_PRIVILEGED_INSTRUCTION && insn[0] == 0xF4) {
        g_veh.traps.fetch_add(1, std::memory_order_relaxed);
        next = uint32_t(c->Rip) + 1;
    } else if (c->SegCs == kCompatCs && r->ExceptionCode == STATUS_ACCESS_VIOLATION && insn[0] == 0x64 && insn[1] == 0xA1) {
        uint64_t addr = r->NumberParameters > 1 ? r->ExceptionInformation[1] : ~0ull;
        if (addr >= 0x1000 && g_veh.bad.fetch_add(1, std::memory_order_relaxed) == 0) {
            g_veh.first_bad_addr = addr;
            g_veh.first_bad_rip = c->Rip;
        }
        uint32_t n = g_veh.faults.fetch_add(1, std::memory_order_relaxed) + 1;
        if ((n & 63) == 0) SwitchToThread();
        next = uint32_t(c->Rip) + 6;
    } else {
        if (g_veh.stray.fetch_add(1, std::memory_order_relaxed) == 0) {
            Fault &f = g_veh.first_stray;
            f = {r->ExceptionCode, c->SegCs, c->Rip, r->NumberParameters > 1 ? r->ExceptionInformation[1] : 0,
                 Access(r->NumberParameters > 0 ? r->ExceptionInformation[0] : 0), c->Rsp, c->Rax};
            memcpy(f.bytes, insn, sizeof f.bytes);
        }
        c->Rip = g_veh.exit64;
        c->SegCs = kLongCs;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    uint32_t esp = uint32_t(c->Rsp) - 8;
    uint32_t *frame = reinterpret_cast<uint32_t *>(static_cast<uintptr_t>(esp));
    frame[0] = next;
    frame[1] = kCompatCs;
    c->Rsp = esp;
    c->Rip = g_veh.resume_cb;
    c->SegCs = kLongCs;
    return EXCEPTION_CONTINUE_EXECUTION;
}

std::string stray_detail()
{
    const Fault &f = g_veh.first_stray;
    std::string s = format(" stray=%u first code=0x%08X cs=0x%02X rip=0x%llX addr=0x%llX kind=%llu rsp=0x%llX rax=0x%llX bytes=",
                           g_veh.stray.load(), f.code, f.cs, static_cast<unsigned long long>(f.rip),
                           static_cast<unsigned long long>(f.addr), static_cast<unsigned long long>(f.kind),
                           static_cast<unsigned long long>(f.rsp), static_cast<unsigned long long>(f.rax));
    for (uint8_t b : f.bytes) s += format("%02X", b);
    return s;
}

struct Result {
    bool ok;
    std::string detail;
};

struct GuestThread {
    ThreadBlock *block = nullptr;
    uint32_t esp = 0;
};

struct Probes {
    GuestMemory *memory;
    CodeSpan code;
    Enter enter = nullptr;
    uint32_t exit64 = 0, far_ptr = 0;
    GuestThread primary;
    LARGE_INTEGER freq{};

    bool thread(GuestThread &t)
    {
        GuestVa block = memory->runtime_alloc(sizeof(ThreadBlock), 0x1000, false);
        GuestMemory::Stack stack = memory->allocate_stack(kStackSize);
        if (block.v == 0 || stack.base.v == 0) return false;
        t.block = memory->host<ThreadBlock>(block);
        t.esp = stack.base.v;
        return true;
    }

    uint32_t run(GuestThread &t, uint32_t payload) { return enter(t.block, t.esp, payload); }

    double ns_per_op(LARGE_INTEGER t0, LARGE_INTEGER t1, uint32_t ops) const
    {
        return double(t1.QuadPart - t0.QuadPart) * 1e9 / double(freq.QuadPart) / double(ops);
    }

    bool emit_gates()
    {
        static_assert(offsetof(ThreadBlock, host_rsp) == 0 && offsetof(ThreadBlock, saved_r14) == 8 &&
                      offsetof(ThreadBlock, saved_r15) == 16);
        Emitter enter64;
        enter64.push_nonvolatiles().mov_r15_rcx().mov_r14_rcx().store_rsp_at_r15().zero_extend_r8d().xor_eax_eax()
            .mov_esp_edx().push_imm8(kCompatCs).push_r8().retfq();
        Emitter exit_gate;
        exit_gate.load_rsp_at_r15().store_r14_at_r15(offsetof(ThreadBlock, saved_r14))
            .store_r15_at_r15(offsetof(ThreadBlock, saved_r15)).pop_nonvolatiles().ret();
        Emitter resume_gate;
        resume_gate.retf();
        Emitter micro;
        micro.mov_esp_esp().retf();
        uint32_t enter_va = code.place(enter64);
        exit64 = code.place(exit_gate);
        uint32_t resume_va = code.place(resume_gate);
        uint32_t micro_va = code.place(micro);
        Emitter ptr;
        ptr.far_pointer(micro_va, kLongCs);
        far_ptr = code.place(ptr);
        if (!enter_va || !exit64 || !resume_va || !micro_va || !far_ptr) return false;
        enter = reinterpret_cast<Enter>(static_cast<uintptr_t>(enter_va));
        g_veh.resume_cb = resume_va;
        g_veh.exit64 = exit64;
        return true;
    }

    Result resume()
    {
        Emitter p;
        p.hlt().mov_eax_imm32(0xBEEF).exit32(exit64);
        uint32_t payload = code.place(p);
        g_veh.reset();
        uint32_t eax = run(primary, payload);
        std::string detail = format("eax=0x%X hits=%u", eax, g_veh.traps.load());
        if (g_veh.stray) detail += stray_detail();
        return {eax == 0xBEEF && g_veh.traps == 1 && g_veh.stray == 0, detail};
    }

    Result selectors()
    {
        const struct {
            const char *name;
            uint8_t modrm;
        } regs[] = {{"fs", 0xE0}, {"gs", 0xE8}, {"ds", 0xD8}, {"ss", 0xD0}};
        std::string detail;
        g_veh.reset();
        for (const auto &r : regs) {
            Emitter p;
            p.xor_eax_eax().mov_eax_sreg(r.modrm).exit32(exit64);
            uint32_t v = run(primary, code.place(p));
            detail += format("%s%s=0x%02X", detail.empty() ? "" : " ", r.name, v & 0xFFFF);
        }
        if (g_veh.stray) detail += stray_detail();
        return {g_veh.stray == 0, detail};
    }

    Result r15()
    {
        Emitter p;
        for (int i = 0; i < 16; i++) p.inc_eax_dec_eax_push_eax_pop_eax();
        p.exit32(exit64);
        uint32_t payload = code.place(p);
        g_veh.reset();
        uint32_t mismatches = 0;
        uint64_t want = reinterpret_cast<uintptr_t>(primary.block);
        for (uint32_t i = 0; i < kTrips; i++) {
            primary.block->saved_r14 = 0;
            primary.block->saved_r15 = 0;
            run(primary, payload);
            if (primary.block->saved_r14 != want || primary.block->saved_r15 != want) mismatches++;
        }
        std::string detail = format("trips=%u mismatches=%u", kTrips, mismatches);
        if (g_veh.stray) detail += stray_detail();
        return {mismatches == 0 && g_veh.stray == 0, detail};
    }

    Result cost()
    {
        Emitter trip;
        trip.exit32(exit64);
        Emitter farcall;
        farcall.mov_ecx_imm32(kCostOps).call_far_ptr(far_ptr).dec_ecx().jnz_rel8(-9).exit32(exit64);
        Emitter trap;
        trap.mov_ecx_imm32(kCostOps).hlt().dec_ecx().jnz_rel8(-4).exit32(exit64);
        uint32_t trip_va = code.place(trip), farcall_va = code.place(farcall), trap_va = code.place(trap);
        g_veh.reset();
        LARGE_INTEGER t0, t1, t2, t3;
        QueryPerformanceCounter(&t0);
        for (uint32_t i = 0; i < kCostOps; i++) run(primary, trip_va);
        QueryPerformanceCounter(&t1);
        run(primary, farcall_va);
        QueryPerformanceCounter(&t2);
        run(primary, trap_va);
        QueryPerformanceCounter(&t3);
        std::string detail = format("roundtrip=%.0f ns farcall=%.0f ns trap=%.0f ns traps=%u", ns_per_op(t0, t1, kCostOps),
                                    ns_per_op(t1, t2, kCostOps), ns_per_op(t2, t3, kCostOps), g_veh.traps.load());
        if (g_veh.stray) detail += stray_detail();
        return {g_veh.traps == kCostOps && g_veh.stray == 0, detail};
    }

    struct FsThread {
        Probes *probes;
        GuestThread guest;
        uint32_t payload;
    };

    static DWORD WINAPI fs_thread(void *p)
    {
        FsThread *t = static_cast<FsThread *>(p);
        t->probes->run(t->guest, t->payload);
        return 0;
    }

    static DWORD WINAPI churn_thread(void *p)
    {
        const std::atomic<bool> *done = static_cast<const std::atomic<bool> *>(p);
        while (!done->load()) Sleep(0);
        return 0;
    }

    Result fs()
    {
        Emitter p;
        p.mov_ecx_imm32(kFsPerThread).mov_eax_fs0().dec_ecx().jnz_rel8(-9).exit32(exit64);
        uint32_t payload = code.place(p);
        FsThread threads[kFsThreads];
        for (FsThread &t : threads) {
            t.probes = this;
            t.payload = payload;
            if (!thread(t.guest)) return {false, "no guest stack or block for a thread"};
        }
        g_veh.reset();
        std::atomic<bool> done{false};
        HANDLE handles[kFsThreads];
        for (uint32_t i = 0; i < kFsThreads; i++) {
            handles[i] = CreateThread(nullptr, 0, fs_thread, &threads[i], CREATE_SUSPENDED, nullptr);
            if (!handles[i]) return {false, format("CreateThread %u err=%lu", i, static_cast<unsigned long>(GetLastError()))};
        }
        HANDLE churn = CreateThread(nullptr, 0, churn_thread, &done, 0, nullptr);
        if (!churn) return {false, format("CreateThread churn err=%lu", static_cast<unsigned long>(GetLastError()))};
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        for (HANDLE h : handles) ResumeThread(h);
        WaitForMultipleObjects(kFsThreads, handles, TRUE, INFINITE);
        QueryPerformanceCounter(&t1);
        done = true;
        WaitForSingleObject(churn, INFINITE);
        for (HANDLE h : handles) CloseHandle(h);
        CloseHandle(churn);
        uint32_t faults = g_veh.faults, bad = g_veh.bad;
        double ms = double(t1.QuadPart - t0.QuadPart) * 1e3 / double(freq.QuadPart);
        std::string detail = format("faults=%u bad=%u threads=%u ms=%.0f", faults, bad, kFsThreads, ms);
        if (bad)
            detail += format(" first bad addr=0x%llX rip=0x%llX", static_cast<unsigned long long>(g_veh.first_bad_addr.load()),
                             static_cast<unsigned long long>(g_veh.first_bad_rip.load()));
        if (g_veh.stray) detail += stray_detail();
        return {bad == 0 && faults == kFsThreads * kFsPerThread && g_veh.stray == 0, detail};
    }
};

}  // namespace

bool run_cpu_probes(GuestMemory &memory, const ProbeSink &sink)
{
    Probes p{&memory};
    QueryPerformanceFrequency(&p.freq);
    constexpr uint32_t span = 0x2000;
    GuestVa code = memory.runtime_alloc(span, 0x1000, true);
    if (code.v == 0) {
        sink({"resume", ProbeLine::State::Fail, "no RWX span in the runtime region"});
        return false;
    }
    p.code = {&memory, code.v, code.v + span, code.v};
    g_veh.code_base = code.v;
    g_veh.code_end = code.v + span;
    if (!p.emit_gates() || !p.thread(p.primary)) {
        sink({"resume", ProbeLine::State::Fail, "no room for the gates, the main ThreadBlock or its stack"});
        return false;
    }
    void *handler = AddVectoredExceptionHandler(1, veh);
    if (!handler) {
        sink({"resume", ProbeLine::State::Fail, format("AddVectoredExceptionHandler err=%lu", static_cast<unsigned long>(GetLastError()))});
        return false;
    }

    const struct {
        const char *name;
        Result (Probes::*run)();
    } probes[] = {
        {"resume", &Probes::resume}, {"selectors", &Probes::selectors}, {"r15", &Probes::r15},
        {"cost", &Probes::cost},     {"fs", &Probes::fs},
    };
    bool all = true;
    for (const auto &probe : probes) {
        sink({probe.name, ProbeLine::State::Begin, ""});
        Result r = (p.*probe.run)();
        sink({probe.name, r.ok ? ProbeLine::State::Ok : ProbeLine::State::Fail, r.detail});
        all = all && r.ok;
    }
    RemoveVectoredExceptionHandler(handler);
    return all;
}

}  // namespace xbrt
