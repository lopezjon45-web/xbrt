#include "host/kernel.h"

#include "core/hle_table.h"
#include "core/survey.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

#if defined(__clang__)
#pragma clang diagnostic error "-Wswitch"
#elif defined(_MSC_VER)
#pragma warning(error : 4062)
#endif

extern "C" {
#include "xbox/xbox_frontend.h"

// The bridges' register file and dispatch (kernel_bridge.c, __declspec(thread) there).
extern __declspec(thread) uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi;
extern __declspec(thread) uint32_t g_xbox_kernel_caller;
int xbox_kernel_bridge_call(uint16_t ordinal);
void xbox_kernel_data_init(void);
uint32_t xbox_kernel_data_export(uint16_t ordinal);
void xbox_path_init(const char *game_dir, const char *save_dir);
void xbox_path_set_title(uint32_t title_id, const char *saves_root);
void xbox_app_guard_writes(const wchar_t *guarded, const wchar_t *const *allowed, int count);
// Read by the bridges' diagnostics and allocation paths; defined below.
extern size_t g_xbox_total_ram;
extern uint32_t g_xbox_code_lo, g_xbox_code_hi;
}

namespace xbrt {

namespace {

constexpr uint32_t kStatusSuccess = 0;
constexpr uint32_t kStatusUnsuccessful = 0xC0000001;
constexpr uint32_t kStatusInvalidParameter = 0xC000000D;
constexpr uint32_t kPageReadWrite = 0x04;
constexpr uint32_t kMemReserve = 0x2000;
constexpr uint32_t kKernelDataSize = 0x1000;

KernelSetup g_setup;
uint32_t g_kernel_data;

// The innermost host call of this thread; a nested guest call starts below its esp.
thread_local GateFrame *t_frame;

// Suspended spawns not yet resumed, by host thread id: their first NtResumeThread is GuestThread::resume().
std::mutex g_suspended_lock;
std::unordered_map<DWORD, GuestThread *> g_suspended;

std::string hex32(uint32_t v)
{
    char buf[16];
    snprintf(buf, sizeof buf, "0x%08X", v);
    return buf;
}

std::string name_site(GuestVa va)
{
    if (g_setup.symbols && g_setup.xbe) return g_setup.symbols->describe(va, *g_setup.xbe);
    return hex32(va.v);
}

uint32_t mem32(uint32_t va) { return *reinterpret_cast<const volatile uint32_t *>(static_cast<uintptr_t>(va)); }

// A NUL-terminated guest string, read only through committed pages. `ok` is false when it runs into unmapped memory.
std::string guest_string(uint32_t va, size_t max, bool &ok)
{
    std::string s;
    ok = true;
    for (size_t i = 0; i < max; i++) {
        if (!xbox_frontend_va_mapped(va + uint32_t(i), 1)) {
            ok = false;
            break;
        }
        char c = *reinterpret_cast<const char *>(static_cast<uintptr_t>(va + i));
        if (!c) break;
        s.push_back(c);
    }
    return s;
}

void stop_or_warn(MissingKind kind, bool stop, std::string what, uint32_t where)
{
    report_missing({kind, stop, std::move(what), {where}, name_site({where}), GetCurrentThreadId()});
}

std::string export_label(Ordinal o)
{
    char buf[24];
    snprintf(buf, sizeof buf, "0x%03X", o.v);
    std::string name(kernel_export_name(o));
    return std::string(buf) + (name.empty() ? "" : " " + name) + " (" + std::to_string(o.v) + ")";
}

void call_kernel(Ordinal o, GateFrame &f) noexcept
{
    uint32_t return_address = mem32(f.esp);
    BridgeStatus status = bridge_status(o);
    if (status == BridgeStatus::Stub) {
        stop_or_warn(MissingKind::KernelExport, true, "Kernel export " + export_label(o) + " is a stub in the kernel bridges",
                     return_address);
        return;
    }

    g_eax = f.eax;
    g_ecx = f.ecx;
    g_edx = f.edx;
    g_esp = f.esp;
    if (!xbox_kernel_bridge_call(o.v)) {
        std::string what = status == BridgeStatus::Data
                               ? "Kernel export " + export_label(o) + " is data, called as a function"
                               : "Missing kernel export " + export_label(o);
        stop_or_warn(MissingKind::KernelExport, true, std::move(what), return_address);
        return;
    }
    f.eax = g_eax;
    f.edx = g_edx;
    f.esp = g_esp;
    f.eip = g_xbox_kernel_caller;
}

uint32_t stack_arg(const GateFrame &f, unsigned n) { return mem32(f.esp + 4 + 4 * n); }

void ret_stdcall(GateFrame &f, HleId id)
{
    f.eip = mem32(f.esp);
    f.esp += 4 + kHleTable[static_cast<size_t>(id)].stdcall_arg_bytes;
}

bool is_dashboard(const std::string &path)
{
    if (path.empty()) return true;
    size_t slash = path.find_last_of("\\/");
    std::string file = path.substr(slash == std::string::npos ? 0 : slash + 1);
    return _stricmp(file.c_str(), "xboxdash.xbe") == 0;
}

void call_override(HleId id, GateFrame &f) noexcept
{
    uint32_t return_address = mem32(f.esp);
    bool ok = true;
    switch (id) {
    case HleId::QueryPerformanceCounter: {
        uint32_t out = stack_arg(f, 0);
        if (!g_setup.cpu) {
            stop_or_warn(MissingKind::Feature, true, "QueryPerformanceCounter before Cpu::install", return_address);
            return;
        }
        if (!xbox_frontend_va_mapped(out, 8)) {
            f.eax = 0;
        } else {
            *reinterpret_cast<uint64_t *>(static_cast<uintptr_t>(out)) = g_setup.cpu->guest_tsc();
            f.eax = 1;
        }
        ret_stdcall(f, id);
        return;
    }
    case HleId::XLaunchNewImageA: {
        uint32_t path_va = stack_arg(f, 0);
        std::string path = path_va ? guest_string(path_va, MAX_PATH, ok) : std::string();
        if (ok && is_dashboard(path)) xbox_frontend_return_to_hub(0);
        stop_or_warn(MissingKind::Feature, true, "XLaunchNewImage(" + path + "): second XBE not supported yet",
                     return_address);
        return;
    }
    case HleId::XapiBootToDash:
        xbox_frontend_return_to_hub(stack_arg(f, 0));
    case HleId::OutputDebugStringA: {
        std::string text = guest_string(stack_arg(f, 0), 4096, ok);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
        g_setup.sink->log("guest: " + text);
        ret_stdcall(f, id);
        return;
    }
    case HleId::Count_:
        break;
    }
}

CallConv call_conv(XboxCallConv c)
{
    switch (c) {
    case XBOX_CALL_STDCALL: return CallConv::Stdcall;
    case XBOX_CALL_CDECL: return CallConv::Cdecl;
    case XBOX_CALL_FASTCALL: return CallConv::Fastcall;
    case XBOX_CALL_THISCALL: return CallConv::Thiscall;
    }
    return CallConv::Stdcall;
}

}  // namespace

std::string_view missing_kind_name(MissingKind k)
{
    switch (k) {
    case MissingKind::KernelExport: return "KernelExport";
    case MissingKind::Instruction: return "Instruction";
    case MissingKind::DeviceRegister: return "DeviceRegister";
    case MissingKind::GpuMethod: return "GpuMethod";
    case MissingKind::Feature: return "Feature";
    case MissingKind::Hang: return "Hang";
    }
    return "?";
}

void report_missing(Missing m) noexcept
{
    if (g_setup.sink) g_setup.sink->missing(m);
}

void report_log(std::string_view line) noexcept
{
    if (g_setup.sink) g_setup.sink->log(line);
}

Parsed<void> kernel_install(const KernelSetup &setup)
{
    if (!setup.memory || !setup.sink) return std::unexpected("kernel: no guest memory or no sink");
    if (setup.game_dir.empty() || setup.scratch_dir.empty() || setup.saves_root.empty())
        return std::unexpected("kernel: no game, scratch or saves folder");
    g_setup = setup;
    GuestVa page = setup.memory->runtime_alloc(kKernelDataSize, kKernelDataSize, false);
    if (!page.v) return std::unexpected("kernel: no room for the kernel data page in the runtime region");
    g_kernel_data = page.v;
    g_xbox_total_ram = setup.memory->ram_bytes();
    g_xbox_code_lo = setup.code_lo.v;
    g_xbox_code_hi = setup.code_hi.v;
    xbox_kernel_data_init();

    auto utf8 = [](const std::u16string &s) {
        std::string out(size_t(WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<LPCWCH>(s.c_str()), -1, nullptr, 0,
                                                   nullptr, nullptr)),
                        '\0');
        WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<LPCWCH>(s.c_str()), -1, out.data(), int(out.size()), nullptr,
                            nullptr);
        if (!out.empty()) out.pop_back();
        return out;
    };
    if (!setup.home.empty()) {
        // INTEGRATION.md section 2: on the drive the runtime writes only its saves and its cache.
        std::u16string saves = setup.home + u"\\saves\\xbrt", cache = setup.home + u"\\cache\\xbrt";
        const wchar_t *allowed[] = {reinterpret_cast<const wchar_t *>(saves.c_str()),
                                    reinterpret_cast<const wchar_t *>(cache.c_str())};
        xbox_app_guard_writes(reinterpret_cast<const wchar_t *>(setup.home.c_str()), allowed, 2);
    }
    xbox_path_init(utf8(setup.game_dir).c_str(), utf8(setup.scratch_dir).c_str());
    xbox_path_set_title(setup.title_id, utf8(setup.saves_root).c_str());
    return {};
}

void on_host_call(GuestThread &thread, HostCallId id, GateFrame &frame) noexcept
{
    (void)thread;
    GateFrame *outer = t_frame;
    t_frame = &frame;
    if (id.is_kernel())
        call_kernel(id.ordinal(), frame);
    else
        call_override(id.hle(), frame);
    t_frame = outer;
}

GuestVa kernel_data_export(Ordinal o) { return {xbox_kernel_data_export(o.v)}; }

}  // namespace xbrt

using xbrt::g_setup;

// ---- The fork's front-end seam (xbox/xbox_frontend.h) ----------------------------------------------------------

extern "C" uint64_t xbox_frontend_guest_call(uint32_t va, unsigned argc, const uint32_t *argv, XboxCallConv conv)
{
    // The bridge that calls guest code resumes on its own register file, and the guest code may make host calls of
    // its own on this thread, which load theirs.
    uint32_t saved[8] = {g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_xbox_kernel_caller};
    xbrt::GateFrame *frame = xbrt::t_frame;
    uint32_t frame_esp = frame ? frame->esp : 0;
    if (frame && g_esp && g_esp < frame->esp) frame->esp = g_esp;
    uint64_t result = xbrt::GuestThread::current().call({va}, {argv, argc}, xbrt::call_conv(conv));
    if (frame) frame->esp = frame_esp;
    g_eax = saved[0], g_ecx = saved[1], g_edx = saved[2], g_esp = saved[3];
    g_ebx = saved[4], g_esi = saved[5], g_edi = saved[6], g_xbox_kernel_caller = saved[7];
    return result;
}

extern "C" void *xbox_frontend_spawn_thread(uint32_t entry, unsigned argc, const uint32_t *argv, uint32_t stack_size,
                                            int priority, int suspended, uint32_t *status)
{
    uint32_t tls = g_setup.xbe ? g_setup.xbe->tls_block_size() : 0;
    xbrt::GuestThread &thread =
        xbrt::GuestThread::spawn({entry}, {argv, argc}, {stack_size, priority, suspended != 0, tls});
    HANDLE h = OpenThread(THREAD_ALL_ACCESS, FALSE, thread.host_thread_id());
    if (!h) {
        *status = xbrt::kStatusUnsuccessful;
        return nullptr;
    }
    if (suspended) {
        std::lock_guard lock(xbrt::g_suspended_lock);
        xbrt::g_suspended[thread.host_thread_id()] = &thread;
    }
    *status = xbrt::kStatusSuccess;
    return h;
}

extern "C" void xbox_frontend_exit_thread(uint32_t exit_status) { xbrt::GuestThread::current().exit(exit_status); }

extern "C" uint32_t xbox_frontend_kpcr(void) { return xbrt::GuestThread::current().kpcr().v; }

extern "C" uint32_t xbox_frontend_kernel_data_base(void) { return xbrt::g_kernel_data; }

// Guest VA == host VA, so the host's own map answers: committed, and neither no-access nor a guard page (device
// pages are no-access).
extern "C" int xbox_frontend_va_mapped(uint32_t va, uint32_t bytes)
{
    uint64_t at = va, end = uint64_t(va) + bytes;
    if (va < 0x1000) return 0;
    while (at < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<void *>(uintptr_t(at)), &mbi, sizeof mbi)) return 0;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
        at = uintptr_t(mbi.BaseAddress) + mbi.RegionSize;
    }
    return 1;
}

extern "C" void xbox_frontend_return_to_hub(uint32_t reason) { g_setup.sink->return_to_hub(reason); }

extern "C" const XboxDiscVolume *xbox_frontend_disc(void) { return g_setup.disc; }

// ---- What the fork's lifted-only files exported, over GuestMemory -------------------------------------------------

extern "C" {

ptrdiff_t g_xbox_mem_offset = 0;
size_t g_xbox_map_size = 0;
size_t g_xbox_total_ram = 0;
uint32_t g_xbox_code_lo, g_xbox_code_hi;

ptrdiff_t xbox_GetMemoryOffset(void) { return 0; }

// Guest VA == physical address for user RAM, and the contiguous view aliases every physical page at 0x80000000 + pa,
// so a device's DMA address always has that guest address.
uint32_t xbox_PhysicalToVirtual(uint32_t pa) { return pa < g_xbox_total_ram ? 0x80000000u + pa : 0; }
size_t xbox_GetMappedSize(void) { return g_xbox_total_ram; }

int guest_vmem_active(void) { return 1; }

int guest_vmem_allocate(uint32_t *base, uint32_t *size, uint32_t alloc_type, uint32_t protect, uint32_t *status)
{
    xbrt::GuestVa va{*base};
    *status = g_setup.memory->allocate_virtual(&va, size, alloc_type, protect);
    *base = va.v;
    return 1;
}

int guest_vmem_free(uint32_t *base, uint32_t *size, uint32_t free_type, uint32_t *status)
{
    xbrt::GuestVa va{*base};
    *status = g_setup.memory->free_virtual(&va, size, free_type);
    *base = va.v;
    return 1;
}

// info is the Xbox MEMORY_BASIC_INFORMATION, seven dwords, which is XboxMemoryBasicInformation's layout.
int guest_vmem_query(uint32_t address, uint32_t info[7])
{
    return g_setup.memory->query_virtual({address}, reinterpret_cast<xbrt::XboxMemoryBasicInformation *>(info)) ==
           xbrt::kStatusSuccess;
}

uint32_t xbox_NtFreeVirtualMemory(uint32_t *base, uint32_t *size, uint32_t free_type)
{
    if (!base || !size) return xbrt::kStatusInvalidParameter;
    uint32_t status = 0;
    guest_vmem_free(base, size, free_type, &status);
    return status;
}

uint32_t xbox_ReserveAlloc(uint32_t size, uint32_t align)
{
    (void)align;
    xbrt::GuestVa va{};
    return g_setup.memory->allocate_virtual(&va, &size, xbrt::kMemReserve, xbrt::kPageReadWrite) == 0 ? va.v : 0;
}

// Pool and MmAllocateSystemMemory. The console hands out zeroed pages and titles rely on it.
uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment)
{
    (void)alignment;
    xbrt::GuestVa va = g_setup.memory->allocate_system(size, xbrt::kPageReadWrite);
    if (va.v) std::memset(g_setup.memory->host<void>(va), 0, size);
    return va.v;
}

void xbox_HeapFree(uint32_t va)
{
    if (va) g_setup.memory->free_system({va}, g_setup.memory->allocation_size({va}));
}

uint32_t xbox_HeapBlockSize(uint32_t va) { return g_setup.memory->allocation_size({va}); }

int xbox_HeapReclaimEnabled(void) { return 1; }

uint32_t xbox_ContiguousAllocRange(uint32_t size, uint32_t lowest, uint32_t highest, uint32_t alignment,
                                   uint32_t protect)
{
    xbrt::GuestVa va = g_setup.memory->allocate_contiguous(size, lowest, highest, alignment, protect);
    if (va.v) std::memset(g_setup.memory->host<void>(va), 0, size);
    return va.v;
}

uint32_t xbox_ContiguousAlloc(uint32_t size, uint32_t alignment)
{
    return xbox_ContiguousAllocRange(size, 0, 0xFFFFFFFF, alignment, xbrt::kPageReadWrite);
}

// Returns whether `va` was contiguous memory; the bridge frees pool memory otherwise.
int xbox_ContiguousFree(uint32_t va)
{
    if (va < 0x80000000u || va >= 0x90000000u) return 0;
    g_setup.memory->free_contiguous({va});
    return 1;
}

uint32_t xbox_ContiguousBlockSize(uint32_t va)
{
    return va >= 0x80000000u && va < 0x90000000u ? g_setup.memory->allocation_size({va}) : 0;
}

uint32_t xbox_MmGetPhysicalAddress(void *va) { return g_setup.memory->physical({uint32_t(uintptr_t(va))}); }

struct XboxMmStatistics {
    uint32_t length, total_physical_pages, available_pages, virtual_bytes_committed, virtual_bytes_reserved,
        cache_pages_committed, pool_pages_committed, stack_pages_committed, image_pages_committed;
};

uint32_t xbox_MmQueryStatistics(XboxMmStatistics *out)
{
    if (!out || out->length != sizeof(XboxMmStatistics)) return xbrt::kStatusInvalidParameter;
    xbrt::GuestMemory::Statistics s = g_setup.memory->statistics();
    *out = {sizeof(XboxMmStatistics), s.total_pages, s.available_pages, s.user_pages * 0x1000u, 0, 0, s.pool_pages,
            s.stack_pages, 0};
    return xbrt::kStatusSuccess;
}

uint32_t xbox_MmQueryAddressProtect(void *va)
{
    uint32_t info[7] = {};
    return guest_vmem_query(uint32_t(uintptr_t(va)), info) ? info[5] : 0;
}

void xbox_MmSetAddressProtect(void *va, uint32_t size, uint32_t protect)
{
    xbrt::GuestVa base{uint32_t(uintptr_t(va))};
    uint32_t old = 0;
    g_setup.memory->protect_virtual(&base, &size, protect, &old);
}

uint32_t xbox_NtProtectVirtualMemory(uint32_t *base, uint32_t *size, uint32_t protect, uint32_t *old_protect)
{
    xbrt::GuestVa va{*base};
    uint32_t status = g_setup.memory->protect_virtual(&va, size, protect, old_protect);
    *base = va.v;
    return status;
}

// Pages here never leave memory, so there is nothing to pin for DMA.
void xbox_MmLockUnlockBufferPages(void *, uint32_t, unsigned char) {}
void xbox_MmLockUnlockPhysicalPage(uintptr_t, unsigned char) {}

// ---- Threads (kernel_thread.c) -------------------------------------------------------------------------------------

uint32_t xbox_KeDelayExecutionThread(char wait_mode, unsigned char alertable, const int64_t *interval)
{
    (void)wait_mode;
    if (!interval) return xbrt::kStatusInvalidParameter;
    int64_t t = *interval;
    if (t == 0) {
        SwitchToThread();
        return xbrt::kStatusSuccess;
    }
    if (t > 0) {
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        int64_t diff = t - int64_t((uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime);
        if (diff <= 0) return xbrt::kStatusSuccess;
        t = -diff;
    }
    DWORD ms = DWORD(-t / 10000);
    if (ms == 0) ms = 1;
    constexpr uint32_t kStatusAlerted = 0x00000101;
    if (alertable) return SleepEx(ms, TRUE) == WAIT_IO_COMPLETION ? kStatusAlerted : xbrt::kStatusSuccess;
    Sleep(ms);
    return xbrt::kStatusSuccess;
}

static VOID CALLBACK alert_apc(ULONG_PTR) {}

uint32_t xbox_KeAlertThread(void *thread, char mode)
{
    (void)mode;
    return QueueUserAPC(alert_apc, static_cast<HANDLE>(thread), 0) ? xbrt::kStatusSuccess : xbrt::kStatusUnsuccessful;
}

uint32_t xbox_NtYieldExecution(void)
{
    SwitchToThread();
    return xbrt::kStatusSuccess;
}

uint32_t xbox_NtSuspendThread(HANDLE thread, uint32_t *previous)
{
    uint32_t count = 0;
    switch (xbrt::suspend_outside_landing(thread, &count)) {
    case xbrt::SuspendResult::Suspended:
        if (previous) *previous = count;
        return xbrt::kStatusSuccess;
    case xbrt::SuspendResult::InsideTrap:
        xbrt::report_missing({xbrt::MissingKind::Hang, true,
                              "NtSuspendThread: the thread stayed inside the trap router (a device register or port access) for 10 ms", {}, "",
                              GetCurrentThreadId()});
        return xbrt::kStatusUnsuccessful;
    case xbrt::SuspendResult::Failed: break;
    }
    return xbrt::kStatusUnsuccessful;
}

uint32_t xbox_NtResumeThread(HANDLE thread, uint32_t *previous)
{
    xbrt::GuestThread *pending = nullptr;
    {
        std::lock_guard lock(xbrt::g_suspended_lock);
        auto it = xbrt::g_suspended.find(GetThreadId(thread));
        if (it != xbrt::g_suspended.end()) {
            pending = it->second;
            xbrt::g_suspended.erase(it);
        }
    }
    if (pending) {
        pending->resume();
        if (previous) *previous = 1;
        return xbrt::kStatusSuccess;
    }
    DWORD prev = ResumeThread(thread);
    if (prev == DWORD(-1)) return xbrt::kStatusUnsuccessful;
    if (previous) *previous = prev;
    return xbrt::kStatusSuccess;
}

// ---- The kernel's own log and the shell contract -------------------------------------------------------------------

void xbox_log(int level, const char *subsystem, const char *fmt, ...)
{
    constexpr int kWarn = 1;
    if (level > kWarn || !g_setup.sink) return;
    char text[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    g_setup.sink->log(std::string("kernel ") + subsystem + ": " + text);
}

// Reached only through SHGetFolderPathW's stand-in when xbox_path_init has no save folder, which kernel_install always
// passes.
const wchar_t *xbox_shell_data_root(void) { return reinterpret_cast<const wchar_t *>(g_setup.scratch_dir.c_str()); }

}  // extern "C"
