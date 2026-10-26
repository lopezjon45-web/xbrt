#include "host/session.h"

#include "core/hle_table.h"
#include "core/plan.h"
#include "core/survey.h"
#include "core/symbols.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <map>
#include <set>
#include <string>

namespace xbrt {

constexpr uint64_t kMaxXbeBytes = 128u << 20;   // the devkit's whole RAM; DiscIndex::read_all draws the same line
constexpr uint64_t kMaxPlanBytes = 64u << 20;
// Thread 0 gets at least this much stack whatever the PE header commits: the kernel builds a trap frame on the guest
// stack at every fault, and the labs commit 64 KB.
constexpr uint32_t kMainStackSize = 0x100000;
constexpr int kMainPriority = 8;   // the Xbox's normal priority
constexpr unsigned kWatchdogSeconds = 10;
constexpr unsigned kCountersLogSeconds = 10;
constexpr Ordinal kDbgPrint{8};
constexpr uint32_t kStopExitCode = 0xDEAD;

std::string_view boot_stage_name(BootStage s)
{
    switch (s) {
    case BootStage::Reserve: return "reserve";
    case BootStage::Link: return "link";
    case BootStage::OpenDisc: return "disc";
    case BootStage::ReadXbe: return "xbe";
    case BootStage::Analyze: return "analyze";
    case BootStage::MapImage: return "map";
    case BootStage::Kernel: return "kernel";
    case BootStage::Devices: return "devices";
    case BootStage::Start: return "start";
    }
    return "?";
}

std::string_view exit_reason_name(ExitReason r)
{
    switch (r) {
    case ExitReason::ReturnedToHub: return "returned-to-hub";
    case ExitReason::GuestExited: return "guest-exited";
    case ExitReason::Fatal: return "fatal";
    }
    return "?";
}

std::string missing_line(const Missing &m)
{
    return std::string(missing_kind_name(m.kind)) + ": " + m.what + (m.where_symbol.empty() ? "" : " at " + m.where_symbol) +
           (m.thread.empty() ? "" : " on " + m.thread);
}

std::string xbe_summary_line(const XbeImage &xbe)
{
    uint16_t build = 0;
    for (const XbeLibrary &l : xbe.libraries)
        if (l.name == "XBOXKRNL" || (build == 0 && l.name == "XAPILIB")) build = l.build;
    char buf[160];
    snprintf(buf, sizeof buf, "%s %s XDK %u, %zu imports", xbe.cert.title_name.c_str(), xbe.title_id_hex().c_str(),
             unsigned(build), xbe.imports.size());
    return buf;
}

namespace {

std::string format(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return buf;
}

std::string thread_label(const GuestThread *t)
{
    return t ? format("t%u", t->index()) : format("host thread %lu", GetCurrentThreadId());
}

struct InFlightCall {
    uint32_t index = 0;
    std::atomic<uint16_t> ordinal{0};
    std::atomic<uint64_t> since_ms{0};
    uint16_t last_call = 0;   // HostCallId::raw()
    uint32_t last_from = 0, last_to = 0;
};
thread_local InFlightCall *t_in_flight = nullptr;

std::string host_call_name(HostCallId id)
{
    if (id.is_kernel()) return format("kernel %u %s", id.ordinal().v, std::string(kernel_export_name(id.ordinal())).c_str());
    return "override " + std::string(kHleTable[static_cast<size_t>(id.hle())].symbol);
}

[[noreturn]] void park_until_process_ends()
{
    for (;;) Sleep(INFINITE);
}

bool never_returns(uint16_t ordinal)
{
    constexpr uint16_t kHalReturnToFirmware = 49, kPsTerminateSystemThread = 258;
    return ordinal == kHalReturnToFirmware || ordinal == kPsTerminateSystemThread;
}

std::string ascii_path(std::u16string_view s)
{
    std::string out;
    for (char16_t c : s) out += c < 128 ? char(c) : '?';
    return out;
}

std::string file_name(std::u16string_view path)
{
    size_t cut = path.find_last_of(u'\\');
    return ascii_path(path.substr(cut == std::u16string_view::npos ? 0 : cut + 1));
}

const wchar_t *wide(const std::u16string &s) { return reinterpret_cast<const wchar_t *>(s.c_str()); }

bool write_atomic(const std::u16string &path, std::span<const uint8_t> bytes)
{
    std::u16string tmp = path + u".tmp";
    FILE *f = _wfopen(wide(tmp), L"wb");
    if (!f) return false;
    bool ok = fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size() && fflush(f) == 0;
    ok = fclose(f) == 0 && ok;
    return ok && MoveFileExW(wide(tmp), wide(path), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

std::optional<std::vector<uint8_t>> read_file(const std::u16string &path, uint64_t max)
{
    FILE *f = _wfopen(wide(path), L"rb");
    if (!f) return std::nullopt;
    std::vector<uint8_t> bytes;
    uint8_t chunk[65536];
    for (size_t n; (n = fread(chunk, 1, sizeof chunk, f)) > 0;) {
        bytes.insert(bytes.end(), chunk, chunk + n);
        if (bytes.size() > max) break;
    }
    fclose(f);
    if (bytes.size() > max) return std::nullopt;
    return bytes;
}

void make_dir(const std::u16string &path) { CreateDirectoryW(wide(path), nullptr); }

std::u16string hex_u16(std::span<const uint8_t> bytes)
{
    std::u16string out;
    for (uint8_t b : bytes) {
        out += u"0123456789abcdef"[b >> 4];
        out += u"0123456789abcdef"[b & 15];
    }
    return out;
}

bool readable(uint32_t va, size_t n)
{
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<void *>(uintptr_t(va)), &info, sizeof info)) return false;
    if (info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    return uint64_t(va) + n <= reinterpret_cast<uint64_t>(info.BaseAddress) + info.RegionSize;
}

uint32_t mem32(uint32_t va) { return *reinterpret_cast<const uint32_t *>(uintptr_t(va)); }

// A NUL-terminated guest string, read page by page through committed memory only; trailing newlines dropped.
std::string guest_text(uint32_t va, size_t max)
{
    std::string s;
    for (size_t i = 0; i < max; i++) {
        uint32_t at = va + uint32_t(i);
        if ((i == 0 || (at & 0xFFF) == 0) && !readable(at, 1)) break;
        char c = *reinterpret_cast<const char *>(uintptr_t(at));
        if (!c) break;
        s += c;
    }
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

std::string counters_line(const CpuCounters &now, const CpuCounters &before)
{
    return format("counters gates/s=%llu guest_calls=%llu traps fs=%llu mmio=%llu port=%llu rdtsc=%llu privileged=%llu "
                  "patch_wait=%llu promoted eager=%llu learned=%llu fs_unpromotable=%llu threads=%u",
                  static_cast<unsigned long long>(now.gate_calls - before.gate_calls), static_cast<unsigned long long>(now.guest_calls),
                  static_cast<unsigned long long>(now.traps_fs), static_cast<unsigned long long>(now.traps_mmio),
                  static_cast<unsigned long long>(now.traps_port), static_cast<unsigned long long>(now.traps_rdtsc),
                  static_cast<unsigned long long>(now.traps_privileged), static_cast<unsigned long long>(now.traps_patch_wait),
                  static_cast<unsigned long long>(now.sites_promoted_eager), static_cast<unsigned long long>(now.sites_promoted_learned),
                  static_cast<unsigned long long>(now.fs_unpromotable), now.live_guest_threads);
}

}  // namespace

struct Session::State final : MissingSink {
    State() = default;
    State(const State &) = delete;
    State &operator=(const State &) = delete;

    ShellServices *shell = nullptr;
    GuestMemory *memory = nullptr;
    std::unique_ptr<Source> image;
    DiscIndex disc;
    XbeImage xbe;
    SymbolMap symbols;
    TitlePlan plan;
    Cpu *cpu = nullptr;
    std::u16string title_dir, plan_path, crash_path;
    bool show_counters = false, trace_kernel = false;
    HANDLE stop_event = nullptr, thread0 = nullptr;
    uint32_t thread0_id = 0;

    SRWLOCK lock = SRWLOCK_INIT;
    SRWLOCK in_flight_lock = SRWLOCK_INIT;
    std::deque<InFlightCall> in_flight;
    std::optional<Missing> stop;
    std::optional<TrapReport> trap;
    std::optional<uint32_t> hub;

    ~State()
    {
        if (stop_event) CloseHandle(stop_event);
        if (thread0) CloseHandle(thread0);
    }

    void record_stop(const Missing &m, const TrapReport *report)
    {
        AcquireSRWLockExclusive(&lock);
        if (!stop) {
            stop = m;
            if (report) trap = *report;
        }
        ReleaseSRWLockExclusive(&lock);
        SetEvent(stop_event);
    }

    InFlightCall &in_flight_of(const GuestThread &t)
    {
        if (!t_in_flight) {
            AcquireSRWLockExclusive(&in_flight_lock);
            t_in_flight = &in_flight.emplace_back();
            t_in_flight->index = t.index();
            ReleaseSRWLockExclusive(&in_flight_lock);
        }
        return *t_in_flight;
    }

    void log_in_flight()
    {
        uint64_t now = GetTickCount64();
        AcquireSRWLockShared(&in_flight_lock);
        for (const InFlightCall &c : in_flight)
            if (uint16_t o = c.ordinal; o && !never_returns(o))
                shell->log(format("in flight at the end of the run: kernel call %u %s on t%u, for %llu ms", o,
                                  std::string(kernel_export_name({o})).c_str(), c.index,
                                  static_cast<unsigned long long>(now - c.since_ms)));
        ReleaseSRWLockShared(&in_flight_lock);
    }

    void missing(const Missing &reported) noexcept override
    {
        Missing m = reported;
        GuestThread *t = GuestThread::current_if_any();
        m.thread = thread_label(t);
        if (!m.stop) {
            shell->log("missing " + missing_line(m));
            return;
        }
        if (t_in_flight && t_in_flight->ordinal) {
            uint16_t o = t_in_flight->ordinal.exchange(0);
            shell->log(format("kernel call %u %s on %s ended by this stop", o, std::string(kernel_export_name({o})).c_str(),
                              m.thread.c_str()));
        }
        record_stop(m, nullptr);
        if (t && t->has_guest_frame()) t->exit(kStopExitCode);
        park_until_process_ends();
    }

    void log(std::string_view line) noexcept override { shell->log(line); }

    [[noreturn]] void return_to_hub(uint32_t reason) noexcept override
    {
        shell->log(format("guest: return to the hub (routine %u)", reason));
        AcquireSRWLockExclusive(&lock);
        if (!hub) hub = reason;
        ReleaseSRWLockExclusive(&lock);
        SetEvent(stop_event);
        GuestThread::current().exit(0);
    }

    ExitReason exit_reason()
    {
        AcquireSRWLockShared(&lock);
        ExitReason r = stop ? ExitReason::Fatal : hub ? ExitReason::ReturnedToHub : ExitReason::GuestExited;
        ReleaseSRWLockShared(&lock);
        return r;
    }

    void write_crash()
    {
        FILE *f = _wfopen(wide(crash_path), L"wb");
        if (!f) {
            shell->log("crash.txt: cannot write " + ascii_path(crash_path));
            return;
        }
        const Missing &m = *stop;
        fprintf(f, "%s\n", missing_line(m).c_str());
        fprintf(f, "kind %s\nwhat %s\nwhere 0x%08X %s\nthread %s\nhost thread %u\n", std::string(missing_kind_name(m.kind)).c_str(),
                m.what.c_str(), m.where.v, m.where_symbol.c_str(), m.thread.c_str(), m.host_thread);
        if (trap) {
            const TrapReport &r = *trap;
            fprintf(f, "thread %u eip 0x%08X\n", r.thread, r.where.v);
            fprintf(f, "eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X ebp=%08X esi=%08X edi=%08X eflags=%08X\n", r.eax, r.ecx, r.edx,
                    r.ebx, r.esp, r.ebp, r.esi, r.edi, r.eflags);
            fprintf(f, "kpcr ExceptionList=%08X StackBase=%08X StackLimit=%08X Irql=%u CurrentThread=%08X TlsData=%08X [TlsData]=%08X\n",
                    r.exception_list, r.stack_base, r.stack_limit, r.irql, r.current_thread, r.tls_data, r.tls_slot0);
            fprintf(f, "stack at esp");
            for (uint32_t v : r.stack_at_esp) fprintf(f, " %08X", v);
            fprintf(f, "\n");
            AcquireSRWLockShared(&in_flight_lock);
            for (const InFlightCall &c : in_flight)
                if (c.index == r.thread && c.last_call)
                    fprintf(f, "last host call on t%u: %s from 0x%08X, resumed at 0x%08X\n", c.index,
                            host_call_name(HostCallId::from_raw(c.last_call)).c_str(), c.last_from, c.last_to);
            ReleaseSRWLockShared(&in_flight_lock);
            fprintf(f, "bytes at eip");
            for (uint8_t b : r.bytes_at_eip) fprintf(f, " %02X", b);
            fprintf(f, "\n");
            for (GuestVa va : r.ebp_chain) fprintf(f, "stack 0x%08X %s\n", va.v, symbols.describe(va, xbe).c_str());
        }
        // The CPU keeps no ring of gate calls yet; its counters are what it has.
        fprintf(f, "%s\n", counters_line(cpu->counters(), CpuCounters{}).c_str());
        fclose(f);
    }

    void save_plan()
    {
        merge_learned(plan, cpu->learned_sites());
        make_dir(shell->data_root() + u"\\titles");
        make_dir(title_dir);
        if (!write_atomic(plan_path, serialize_plan(plan)))
            shell->log("plan.bin: cannot write " + ascii_path(plan_path) + format(" (error %lu)", GetLastError()));
        else
            shell->log(format("plan.bin: %zu sites written", plan.sites.size()));
    }
};

namespace {

// Cpu::install takes plain function pointers, and both it and kernel_install run once per process, so the one
// session's State is reachable from here.
Session::State *g_state = nullptr;

// DbgPrint is the one kernel export the session answers itself: the fork's bridge prints it to stderr, which the
// console has no reader for, and only within its log budget. Here it reaches run.txt like OutputDebugStringA. The
// text is a guest-owned format string, logged verbatim, never formatted.
void host_call(GuestThread &thread, HostCallId id, GateFrame &frame) noexcept
{
    Session::State &s = *g_state;
    if (id.is_kernel() && id.ordinal() == kDbgPrint) {
        s.shell->log("guest: " + guest_text(mem32(frame.esp + 4), 512));
        frame.eax = 0;
        frame.eip = mem32(frame.esp);
        frame.esp += 4;
        return;
    }
    InFlightCall &call = s.in_flight_of(thread);
    if (!id.is_kernel()) {
        call.last_from = mem32(frame.esp);
        on_host_call(thread, id, frame);
        call.last_call = id.raw();
        call.last_to = frame.eip;
        return;
    }
    uint16_t o = id.ordinal().v;
    uint64_t outer_since = call.since_ms.exchange(GetTickCount64());
    uint16_t outer = call.ordinal.exchange(o);
    if (s.trace_kernel) {
        // NtCreateFile (190) and NtOpenFile (202) take POBJECT_ATTRIBUTES as their third argument; its ObjectName at
        // +4 is an ANSI_STRING {Length, MaximumLength, Buffer}.
        std::string name;
        uint32_t attributes = (o == 190 || o == 202) ? mem32(frame.esp + 12) : 0;
        if (attributes && readable(attributes, 8)) {
            uint32_t object_name = mem32(attributes + 4);
            if (object_name && readable(object_name, 8))
                name = " name=\"" + guest_text(mem32(object_name + 4), mem32(object_name) & 0xFFFF) + "\"";
        }
        s.shell->log(format("kernel call %u %s(%08X, %08X, %08X, %08X)%s from %s on t%u", o,
                            std::string(kernel_export_name(id.ordinal())).c_str(), mem32(frame.esp + 4), mem32(frame.esp + 8),
                            mem32(frame.esp + 12), mem32(frame.esp + 16), name.c_str(),
                            s.symbols.describe({mem32(frame.esp)}, s.xbe).c_str(), thread.index()));
    }
    uint32_t from = mem32(frame.esp);
    on_host_call(thread, id, frame);
    call.last_call = id.raw();
    call.last_from = from;
    call.last_to = frame.eip;
    if (s.trace_kernel) s.shell->log(format("kernel call %u returned eax=%08X on t%u", o, frame.eax, thread.index()));
    call.ordinal = outer;
    call.since_ms = outer_since;
}

// The trap router's fatal path: record and signal; the router unwinds the faulting thread itself.
void on_fatal(const TrapReport &r) noexcept
{
    Session::State &s = *g_state;
    s.record_stop({MissingKind::Instruction, true, r.what, r.where, s.symbols.describe(r.where, s.xbe), GetCurrentThreadId(),
                   thread_label(GuestThread::current_if_any())},
                  &r);
}

}  // namespace

Session::Session() = default;
Session::Session(Session &&) noexcept = default;
Session &Session::operator=(Session &&) noexcept = default;

Session::~Session()
{
    // After a Hang the guest threads still run and still reach g_state through host_call; the process is about to
    // exit, so the State outlives the Session rather than dangling under them.
    if (s_ && s_->cpu && s_->cpu->counters().live_guest_threads > 0) (void)s_.release();
}

const XbeImage &Session::xbe() const { return s_->xbe; }
const DiscIndex &Session::disc() const { return s_->disc; }
const Missing *Session::stop() const { return s_->stop ? &*s_->stop : nullptr; }

std::expected<Session, BootError> Session::boot(const LaunchRequest &request, GuestMemory &memory, ShellServices &shell)
{
    auto failed = [&shell](BootStage stage, std::string detail) {
        shell.fail(stage, detail);
        return std::unexpected(BootError{stage, std::move(detail)});
    };
    Session session;
    session.s_ = std::make_unique<State>();
    State &s = *session.s_;
    s.shell = &shell;
    s.memory = &memory;
    s.show_counters = request.show_counters;
    s.trace_kernel = request.trace == Trace::Kernel;
    s.stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!s.stop_event) return failed(BootStage::OpenDisc, format("CreateEvent failed (error %lu)", GetLastError()));
    std::string error;
    char line[256];
    std::vector<uint8_t> file;
    std::u16string game_dir;
    if (!request.dir_path.empty()) {
        // An extracted disc: default.xbe is a plain file under the directory, and there is no index to build.
        std::u16string dir = request.dir_path;
        if (dir.size() < 3 || dir[1] != u':') dir = shell.data_root() + u"\\" + dir;
        std::u16string path = dir + request.xbe;
        std::unique_ptr<Source> xbe_file = shell.open_disc(path, error);
        if (!xbe_file) return failed(BootStage::OpenDisc, ascii_path(path) + ": " + error);
        if (xbe_file->size() > kMaxXbeBytes) return failed(BootStage::ReadXbe, ascii_path(path) + ": larger than any XBE");
        file.resize(size_t(xbe_file->size()));
        if (!xbe_file->read_at(0, file)) return failed(BootStage::ReadXbe, ascii_path(path) + ": read failed");
        snprintf(line, sizeof line, "disc %s (extracted)", ascii_path(dir).c_str());
        shell.stage(BootStage::OpenDisc, line);
        game_dir = dir;
    } else {
        s.image = shell.open_disc(request.disc_path, error);
        if (!s.image) return failed(BootStage::OpenDisc, ascii_path(request.disc_path) + ": " + error);
        Parsed<DiscIndex> index = DiscIndex::open(*s.image);
        if (!index) return failed(BootStage::OpenDisc, index.error());
        s.disc = std::move(*index);
        snprintf(line, sizeof line, "disc %s (%zu entries)", ascii_path(request.disc_path).c_str(), s.disc.all().size());
        shell.stage(BootStage::OpenDisc, line);
        std::string xbe_path = ascii_path(request.xbe);
        std::optional<DiscFile> f = s.disc.find(xbe_path);
        if (!f) return failed(BootStage::ReadXbe, xbe_path + " is not on the disc");
        Parsed<std::vector<uint8_t>> bytes = s.disc.read_all(*f);
        if (!bytes) return failed(BootStage::ReadXbe, bytes.error());
        file = std::move(*bytes);
    }
    Parsed<XbeImage> xbe = parse_xbe(file);
    if (!xbe) return failed(BootStage::ReadXbe, xbe.error());
    s.xbe = std::move(*xbe);
    shell.stage(BootStage::ReadXbe, "xbe " + xbe_summary_line(s.xbe));

    s.symbols = scan_symbols(s.xbe, file);
    s.plan = plan_title(s.xbe, file, s.symbols);
    s.title_dir = shell.data_root() + u"\\titles\\" + hex_u16(s.xbe.sha256);
    s.plan_path = s.title_dir + u"\\plan.bin";
    s.crash_path = shell.data_root() + u"\\crash.txt";
    std::string cache = "no plan.bin";
    if (std::optional<std::vector<uint8_t>> cached = read_file(s.plan_path, kMaxPlanBytes)) {
        Parsed<TitlePlan> plan = deserialize_plan(*cached, s.xbe.sha256);
        if (plan) {
            size_t before = s.plan.sites.size();
            merge_learned(s.plan, plan->sites);
            cache = format("plan.bin +%zu learned sites", s.plan.sites.size() - before);
        } else {
            cache = plan.error() + ", ignored";
        }
    }
    const CodeMapStats &map = s.plan.code_map;
    shell.stage(BootStage::Analyze, format("analyze %zu symbols, %u fs sites (%u unpromotable), %u rdtsc, %u privileged, %zu overrides, %s",
                                           s.symbols.all().size(), map.fs_sites, map.fs_unpromotable, map.rdtsc_sites, map.privileged_sites,
                                           s.plan.overrides.size(), cache.c_str()));
    for (const std::string &note : s.plan.notes) shell.log("analyze: " + note);

    if (Parsed<void> mapped = memory.map_image(s.xbe, file); !mapped) return failed(BootStage::MapImage, mapped.error());
    shell.stage(BootStage::MapImage, format("map 0x%08X +0x%X, %zu sections", s.xbe.base.v, s.xbe.image_size, s.xbe.sections.size()));

    if (game_dir.empty()) return failed(BootStage::Kernel, "Feature: disc images need the XISO volume (U3); boot with dir=");
    CpuOptions options;
    options.trace_traps = request.trace == Trace::Traps;
    g_state = &s;
    s.cpu = &Cpu::install(memory, host_call, on_fatal, options);
    GuestVa code_lo{~0u}, code_hi{};
    for (const XbeSection &sec : s.xbe.sections) {
        if (!sec.executable) continue;
        code_lo = std::min(code_lo, sec.va);
        code_hi = std::max(code_hi, sec.va + sec.virtual_size);
    }
    if (code_lo > code_hi) code_lo = code_hi;
    std::string title_id = s.xbe.title_id_hex();
    std::u16string title_id16(title_id.begin(), title_id.end());
    std::u16string save_dir = request.home.empty() ? shell.data_root() + u"\\titles\\" + title_id16 : request.home + u"\\saves\\xbrt\\" + title_id16;
    KernelSetup setup{&memory, &s, s.cpu, game_dir, save_dir, code_lo, code_hi, &s.symbols, &s.xbe};
    if (Parsed<void> installed = kernel_install(setup); !installed) return failed(BootStage::Kernel, installed.error());
    size_t data_slots = 0;
    for (const ImportSlot &slot : s.plan.imports) {
        GuestVa data = kernel_data_export(slot.ordinal);
        data_slots += data.v != 0;
        *memory.host<uint32_t>(slot.slot) = data.v ? data.v : s.cpu->gate_for(HostCallId::kernel(slot.ordinal)).v;
    }
    s.cpu->apply_overrides(s.plan.overrides);
    std::vector<PatchSite> skipped = s.cpu->apply_sites(s.plan.sites);
    std::set<uint32_t> left;
    for (const PatchSite &site : skipped) left.insert(site.at.v);
    std::vector<GuestVa> rdtsc;
    for (const PatchSite &site : s.plan.sites)
        if (site.kind == SiteKind::Rdtsc && !left.count(site.at.v)) rdtsc.push_back(site.at);
    s.cpu->note_rdtsc_sites(rdtsc);
    shell.stage(BootStage::Kernel, format("kernel %zu imports (%zu data), %zu overrides, %zu sites patched, %zu left to the trap router",
                                          s.plan.imports.size(), data_slots, s.plan.overrides.size(), s.plan.sites.size() - skipped.size(),
                                          skipped.size()));

    shell.attach_devices(*s.cpu);
    shell.stage(BootStage::Devices, "devices none (the APU, OHCI and NV2A come with U4 and U5)");

    uint32_t tls_size = s.xbe.tls_block_size();
    uint32_t stack = std::max(s.xbe.stack_commit, kMainStackSize);
    GuestThread &thread0 = GuestThread::spawn(s.xbe.entry, {}, {stack, kMainPriority, true, tls_size});
    s.thread0_id = thread0.host_thread_id();
    s.thread0 = OpenThread(SYNCHRONIZE, FALSE, s.thread0_id);
    if (!s.thread0) return failed(BootStage::Start, format("OpenThread failed for thread 0 (error %lu)", GetLastError()));
    thread0.resume();
    shell.stage(BootStage::Start, format("start thread 0 at 0x%08X (%s), stack 0x%X, tls 0x%X", s.xbe.entry.v,
                                         s.symbols.describe(s.xbe.entry, s.xbe).c_str(), stack, tls_size));
    return session;
}

ExitReason Session::run()
{
    State &s = *s_;
    CpuCounters before = s.cpu->counters();
    unsigned seconds = 0, idle = 0;
    bool thread0_ended = false;
    for (;;) {
        if (WaitForSingleObject(s.stop_event, 1000) != WAIT_TIMEOUT) break;
        seconds++;
        if (!thread0_ended && WaitForSingleObject(s.thread0, 0) == WAIT_OBJECT_0) {
            thread0_ended = true;
            DWORD code = 0;
            GetExitCodeThread(s.thread0, &code);
            s.shell->log(format("thread t0 ended (exit code 0x%lX) within %u s, %u guest threads running", code, seconds,
                                s.cpu->counters().live_guest_threads));
        }
        CpuCounters now = s.cpu->counters();
        std::string line = counters_line(now, before);
        if (s.show_counters) s.shell->counters(line);
        if (seconds % kCountersLogSeconds == 0) s.shell->log(line);
        idle = now.gate_calls == before.gate_calls ? idle + 1 : 0;
        before = now;
        if (now.live_guest_threads == 0) break;
        if (idle >= kWatchdogSeconds) {
            // The thread states (what each one waits on, where each one spins) come with U3's suspend-and-read.
            s.record_stop({MissingKind::Hang, true,
                           format("no kernel call for %u s (%u guest threads; thread 0 is host thread %u)", kWatchdogSeconds,
                                  now.live_guest_threads, s.thread0_id),
                           {}, "", s.thread0_id, "the watchdog"},
                          nullptr);
            break;
        }
    }
    ExitReason reason = s.exit_reason();
    s.log_in_flight();
    if (reason == ExitReason::Fatal) s.write_crash();
    s.save_plan();
    return reason;
}

Parsed<SurveyTotals> run_survey(std::u16string_view folder, ShellServices &shell,
                                const std::function<void(std::string_view)> &progress)
{
    std::string error;
    std::vector<std::u16string> images = shell.list_discs(folder, error);
    if (!error.empty()) return std::unexpected(error);
    if (images.empty()) return std::unexpected("no .iso or .xiso in " + file_name(folder));

    SurveyTotals totals{};
    totals.images = uint32_t(images.size());
    std::vector<SurveyRow> rows;
    std::map<Ordinal, uint32_t> no_bridge;
    std::set<uint16_t> builds;
    std::u16string tsv_path = shell.data_root() + u"\\survey.tsv";
    std::string tsv = tsv_header();
    for (size_t i = 0; i < images.size(); i++) {
        std::string name = file_name(images[i]);
        progress("survey " + std::to_string(i + 1) + "/" + std::to_string(images.size()) + " " + name);
        std::unique_ptr<Source> image = shell.open_disc(images[i], error);
        SurveyRow row;
        if (image) {
            row = survey_disc(*image, name);
        } else {
            row.image_path = name;
            row.reason = error;
        }
        switch (row.verdict) {
        case SurveyVerdict::Ready: totals.ready++; break;
        case SurveyVerdict::NamedGaps: totals.named_gaps++; break;
        case SurveyVerdict::Unparsed: totals.unparsed++; break;
        }
        if (row.verdict != SurveyVerdict::Unparsed) {
            totals.parsed++;
            for (Ordinal o : row.imports_no_bridge) no_bridge[o]++;
            for (const LibraryCoverage &l : row.libraries) builds.insert(l.library.build);
        }
        progress("survey " + std::to_string(i + 1) + "/" + std::to_string(images.size()) + " " + name + ": " +
                 std::string(survey_verdict_name(row.verdict)) + (row.reason.empty() ? "" : " " + row.reason));
        tsv += to_tsv_line(row);
        if (!write_atomic(tsv_path, {reinterpret_cast<const uint8_t *>(tsv.data()), tsv.size()}))
            return std::unexpected("survey.tsv: cannot write it under LocalState (error " + std::to_string(GetLastError()) + ")");
    }
    totals.distinct_builds = uint32_t(builds.size());
    for (const auto &[o, n] : no_bridge) totals.no_bridge_by_titles.push_back({o, n});
    std::sort(totals.no_bridge_by_titles.begin(), totals.no_bridge_by_titles.end(),
              [](const auto &a, const auto &b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
    return totals;
}

}  // namespace xbrt
