#include "host/cpu/cpu_internal.h"

#include <algorithm>
#include <cstring>

namespace xbrt {

namespace detail {

CpuState *g_cpu = nullptr;
thread_local ThreadBlock *t_block = nullptr;

struct ThreadAccess {
    static GuestThread *make(ThreadBlock *b) { return new GuestThread(b); }
    static ThreadBlock *block(GuestThread &t) { return t.block_; }
};

ThreadBlock *block_of(GuestThread &t) { return ThreadAccess::block(t); }
HANDLE thread_handle(GuestThread &t) { return static_cast<HANDLE>(block_of(t)->host.host_thread); }

struct SpawnArgs {
    uint32_t entry;
    std::vector<uint32_t> args;
};

namespace {

constexpr uint32_t kMainStackSize = 0x100000;
constexpr uint32_t kCallGap = 0x40;
constexpr DWORD_PTR kPreferredGuestCore = DWORD_PTR(1) << 2;

uint8_t *host(uint32_t va) { return reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(va)); }

uint32_t place(uint32_t at, const std::vector<uint8_t> &bytes)
{
    memcpy(host(at), bytes.data(), bytes.size());
    return at;
}

[[noreturn]] void install_failed(const char *what, Cpu::FatalSink sink)
{
    TrapReport r{};
    r.what = what;
    sink(r);
    TerminateProcess(GetCurrentProcess(), 3);
    for (;;) {}
}

// All guest threads on one core, as on the console (design.md decision 6, xboxrecomp's xbox_PinToGuestCore rule).
void pin_to_guest_core()
{
    static DWORD_PTR mask = 0;
    static std::once_flag once;
    std::call_once(once, [] {
        DWORD_PTR proc = 0, sys = 0;
        if (GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys) && proc)
            mask = (proc & kPreferredGuestCore) ? kPreferredGuestCore : (proc & (~proc + 1));
    });
    if (mask) SetThreadAffinityMask(GetCurrentThread(), mask);
}

ThreadBlock *make_thread(uint32_t stack_size, uint32_t tls_data_size, int priority)
{
    GuestMemory &m = *g_cpu->memory;
    GuestVa bva = m.alloc_thread_block();
    GuestMemory::Stack stack = m.allocate_stack(stack_size);
    if (!bva.v || !stack.base.v) return nullptr;
    ThreadBlock *tb = m.host<ThreadBlock>(bva);
    memset(tb, 0, sizeof *tb);
    XboxKpcr &k = tb->kpcr;
    k.NtTib.StackBase = stack.base.v;
    k.NtTib.StackLimit = stack.limit.v;
    k.NtTib.Self = bva.v;
    k.SelfPcr = bva.v;
    k.Prcb = bva.v + offsetof(XboxKpcr, PrcbData);
    k.Irql = 0;
    k.PrcbData.CurrentThread = bva.v + kEthreadOffset;
    k.PrcbData.DpcListHead[0] = k.PrcbData.DpcListHead[1] = bva.v + offsetof(XboxKpcr, PrcbData.DpcListHead);
    k.PrcbData.DmEnetFunc = g_cpu->gates.ret_stub;
    XboxEthread &e = tb->ethread;
    e.Tcb.StackBase = stack.base.v;
    e.Tcb.StackLimit = stack.limit.v;
    e.Tcb.KernelStack = stack.base.v;
    e.Tcb.State = kKthreadRunning;
    e.Tcb.Priority = e.Tcb.BasePriority = uint8_t(priority);
    if (tls_data_size) {
        uint32_t tls = bva.v + kTlsOffset;
        if (tls_data_size + 4 > kTlsCapacity) tls = m.allocate_system(tls_data_size + 4, xbox_page::ReadWrite).v;
        if (!tls) return nullptr;
        *m.host<uint32_t>({tls}) = tls + 4;
        e.Tcb.TlsData = tls;
    }
    HostPart &h = tb->host;
    h.host_mxcsr = h.guest_mxcsr = kAbiMxcsr;
    h.host_fpcw = h.guest_fpcw = kAbiFpcw;
    h.stack_base = stack.base.v;
    h.stack_limit = stack.limit.v;
    h.index = g_cpu->next_index++;
    std::lock_guard<std::mutex> hold(g_cpu->threads_lock);
    g_cpu->threads.emplace_back(ThreadAccess::make(tb));
    h.thread = g_cpu->threads.back().get();
    return tb;
}

void adopt(ThreadBlock *tb)
{
    t_block = tb;
    tb->host.host_thread_id = GetCurrentThreadId();
    tb->ethread.UniqueThread = tb->host.host_thread_id;
    if (g_cpu->options.pin_guest_threads_to_one_core) pin_to_guest_core();
}

DWORD WINAPI guest_thread_main(void *p)
{
    ThreadBlock *tb = static_cast<ThreadBlock *>(p);
    adopt(tb);
    g_cpu->guest_threads++;
    std::unique_ptr<SpawnArgs> spawn(tb->host.spawn);
    tb->host.spawn = nullptr;
    GuestThread &t = *tb->host.thread;
    uint64_t r = t.call({spawn->entry}, spawn->args, CallConv::Stdcall);
    g_cpu->guest_threads--;
    g_cpu->memory->free_stack({{tb->host.stack_base}, {tb->host.stack_limit}});
    return uint32_t(r);
}

}  // namespace

uint64_t guest_tsc_now()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    uint64_t hz = uint64_t(g_cpu->options.guest_tsc_hz), f = uint64_t(g_cpu->qpf.QuadPart), t = uint64_t(now.QuadPart);
    return (t / f) * hz + ((t % f) * hz) / f;
}

uint32_t write_far_pointer(uint32_t offset, PatchLockHeld)
{
    if (g_cpu->far_ptr_cursor + 8 > kIdStubsOffset) return 0;
    uint32_t at = g_cpu->chunk + g_cpu->far_ptr_cursor;
    place(at, emit_far_pointer(offset, kLongCs));
    g_cpu->far_ptr_cursor += 8;
    return at;
}

extern "C" void xbrt_on_gate(GateFrame *frame, ThreadBlock *tb) noexcept
{
    g_cpu->gate_calls++;
    if (tb->host.depth == 1) tb->host.base_rsp = tb->host.host_rsp;
    tb->host.guest_esp = frame->esp;
    // A guest callback the handler runs inherits the caller's FP control state, as on the Xbox; full_gate64
    // restores the frame's copy for the caller afterwards.
    tb->host.guest_mxcsr = frame->mxcsr;
    tb->host.guest_fpcw = frame->fpcw;
    g_cpu->on_host_call(*tb->host.thread, HostCallId::from_raw(uint16_t(frame->eax)), *frame);
}

// Jumps into exit64 with the base frame's rsp: the host frames between (the bridges' C frames and the nested
// enter64 frames) are abandoned, and the outermost call() returns `code`.
[[noreturn]] void unwind_thread(ThreadBlock *tb, uint32_t code)
{
    tb->host.exited = 1;
    tb->host.exit_code = code;
    tb->host.depth = 1;
    reinterpret_cast<UnwindFn>(static_cast<uintptr_t>(g_cpu->gates.unwind64))(tb, code);
    for (;;) {}
}

}  // namespace detail

using namespace detail;

Cpu &Cpu::install(GuestMemory &memory, HostCallHandler on_host_call, FatalSink on_fatal, const CpuOptions &options)
{
    static Cpu cpu;
    if (g_cpu) return cpu;
    auto *s = new CpuState;
    s->memory = &memory;
    s->on_host_call = on_host_call;
    s->on_fatal = on_fatal;
    s->options = options;
    QueryPerformanceFrequency(&s->qpf);
    GuestVa chunk = memory.runtime_alloc(kChunkSize, 0x10000, true);
    if (!chunk.v) install_failed("runtime region: no 1 MB RWX chunk for the gates", on_fatal);
    s->chunk = chunk.v;
    g_cpu = s;

    uint32_t cursor = chunk.v;
    auto put = [&cursor](const std::vector<uint8_t> &bytes) {
        uint32_t at = (cursor + 15) & ~15u;
        cursor = at + uint32_t(bytes.size());
        return place(at, bytes);
    };
    Gates &g = s->gates;
    g.enter64 = put(emit_enter64());
    g.exit64 = put(emit_exit64());
    g.full_gate64 = put(emit_full_gate64(reinterpret_cast<uint64_t>(&xbrt_on_gate)));
    g.resume64 = put(emit_resume64());
    g.unwind64 = put(emit_unwind64(g.exit64));
    g.ret_stub = put(emit_ret_stub());
    g.fault_gate = put(emit_fault_gate());
    {
        std::lock_guard<std::mutex> hold(s->patch_lock);
        g.ptr_exit64 = write_far_pointer(g.exit64, hold);
        g.ptr_full_gate64 = write_far_pointer(g.full_gate64, hold);
    }
    g.exit32 = put(emit_exit32(g.ptr_exit64));
    if (cursor > chunk.v + kVariantsOffset) install_failed("the fixed gates overflow their page", on_fatal);
    FlushInstructionCache(GetCurrentProcess(), host(chunk.v), kChunkSize);
    s->id_stub_made.assign((kChunkSize - kIdStubsOffset) / kIdStubSize, false);
    if (!AddVectoredExceptionHandler(1, cpu_veh)) install_failed("AddVectoredExceptionHandler failed", on_fatal);
    return cpu;
}

GuestVa Cpu::gate_for(HostCallId id)
{
    uint32_t raw = id.raw();
    if (raw >= g_cpu->id_stub_made.size()) return {};
    uint32_t at = g_cpu->chunk + kIdStubsOffset + raw * kIdStubSize;
    std::lock_guard<std::mutex> hold(g_cpu->patch_lock);
    if (!g_cpu->id_stub_made[raw]) {
        place(at, emit_id_stub(uint16_t(raw), g_cpu->gates.ptr_full_gate64));
        FlushInstructionCache(GetCurrentProcess(), host(at), kIdStubSize);
        g_cpu->id_stub_made[raw] = true;
    }
    return {at};
}

std::vector<PatchSite> Cpu::learned_sites() const
{
    std::lock_guard<std::mutex> hold(g_cpu->patch_lock);
    return g_cpu->learned;
}

void Cpu::map_mmio(GuestRange range, MmioDevice &device)
{
    std::lock_guard<std::mutex> hold(g_cpu->routes_lock);
    g_cpu->mmio.push_back({range.base.v, range.base.v + range.size, &device});
    std::sort(g_cpu->mmio.begin(), g_cpu->mmio.end(), [](const MmioMap &a, const MmioMap &b) { return a.base < b.base; });
    g_cpu->memory->set_no_access(range.base.v, range.size);
}

void Cpu::map_ports(PortRange range, PortDevice &device)
{
    std::lock_guard<std::mutex> hold(g_cpu->routes_lock);
    g_cpu->ports.push_back({range.first, uint16_t(range.first + range.count), &device});
    std::sort(g_cpu->ports.begin(), g_cpu->ports.end(), [](const PortMap &a, const PortMap &b) { return a.first < b.first; });
}

void Cpu::note_rdtsc_sites(std::span<const GuestVa> sites)
{
    std::lock_guard<std::mutex> hold(g_cpu->routes_lock);
    for (GuestVa va : sites) g_cpu->rdtsc_sites.insert(va.v);
}

CpuCounters Cpu::counters() const
{
    CpuState &s = *g_cpu;
    return {s.gate_calls, s.guest_calls, s.traps_fs, s.traps_mmio, s.traps_port, s.traps_rdtsc, s.traps_privileged,
            s.traps_patch_wait, s.sites_promoted_eager, s.sites_promoted_learned, s.fs_unpromotable, s.guest_threads};
}

uint64_t Cpu::guest_tsc() const { return guest_tsc_now(); }

GuestThread &GuestThread::current()
{
    if (!t_block) {
        ThreadBlock *tb = make_thread(kMainStackSize, 0, 8);
        if (!tb) install_failed("no ThreadBlock or guest stack for the calling thread", g_cpu->on_fatal);
        adopt(tb);
    }
    return *t_block->host.thread;
}

GuestThread &GuestThread::spawn(GuestVa entry, std::span<const uint32_t> args, const SpawnOptions &options)
{
    ThreadBlock *tb = make_thread(options.stack_size, options.tls_data_size, options.priority);
    if (!tb) install_failed("no ThreadBlock or guest stack for a spawned thread", g_cpu->on_fatal);
    tb->host.spawn = new SpawnArgs{entry.v, std::vector<uint32_t>(args.begin(), args.end())};
    tb->host.suspended = options.suspended;
    DWORD id = 0;
    HANDLE h = CreateThread(nullptr, 0, guest_thread_main, tb, CREATE_SUSPENDED, &id);
    if (!h) install_failed("CreateThread failed for a spawned guest thread", g_cpu->on_fatal);
    tb->host.host_thread = h;
    tb->host.host_thread_id = id;
    tb->ethread.UniqueThread = id;
    if (!options.suspended) ResumeThread(h);
    return *tb->host.thread;
}

uint64_t GuestThread::call(GuestVa va, std::span<const uint32_t> args, CallConv conv)
{
    ThreadBlock *tb = block_;
    HostPart &h = tb->host;
    uint32_t esp = h.depth == 0 ? h.stack_base - 16 : (h.guest_esp - kCallGap) & ~15u;
    uint32_t ecx = 0, edx = 0;
    size_t first_on_stack = 0;
    switch (conv) {
    case CallConv::Stdcall:
    case CallConv::Cdecl: break;
    case CallConv::Fastcall:
        if (args.size() > 0) ecx = args[0];
        if (args.size() > 1) edx = args[1];
        first_on_stack = std::min<size_t>(args.size(), 2);
        break;
    case CallConv::Thiscall:
        if (args.size() > 0) ecx = args[0];
        first_on_stack = std::min<size_t>(args.size(), 1);
        break;
    }
    uint32_t on_stack = uint32_t(args.size() - first_on_stack);
    esp -= 4 * on_stack;
    memcpy(host(esp), args.data() + first_on_stack, 4 * on_stack);
    esp -= 4;
    *reinterpret_cast<uint32_t *>(host(esp)) = g_cpu->gates.exit32;
    EntryState st{esp, va.v, ecx, edx};
    h.depth++;
    g_cpu->guest_calls++;
    uint64_t r = reinterpret_cast<EnterFn>(static_cast<uintptr_t>(g_cpu->gates.enter64))(tb, &st);
    h.depth--;
    if (h.exited && h.depth == 0) {
        // exit() or a fatal trap unwound to this base frame: the host thread ends here (the main thread included).
        if (h.host_thread) {
            g_cpu->guest_threads--;
            g_cpu->memory->free_stack({{h.stack_base}, {h.stack_limit}});
        }
        ExitThread(h.exit_code);
    }
    return r;
}

void GuestThread::exit(uint32_t code) { unwind_thread(block_, code); }

GuestVa GuestThread::kpcr() const { return {uint32_t(reinterpret_cast<uintptr_t>(block_))}; }
GuestVa GuestThread::ethread() const { return {uint32_t(reinterpret_cast<uintptr_t>(block_)) + kEthreadOffset}; }
uint32_t GuestThread::index() const { return block_->host.index; }
uint32_t GuestThread::host_thread_id() const { return block_->host.host_thread_id; }

void GuestThread::resume()
{
    if (block_->host.suspended && block_->host.host_thread) {
        block_->host.suspended = 0;
        ResumeThread(static_cast<HANDLE>(block_->host.host_thread));
    }
}

}  // namespace xbrt
