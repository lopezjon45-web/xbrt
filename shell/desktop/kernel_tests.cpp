// xbrt_desktop.exe --kernel-tests: host/kernel.cpp's on_host_call against the fork's bridges, through hand-built
// GateFrames on a guest stack in system space. No guest code runs. One line per test, "kernel <name> ok|FAIL <detail>".
#include "shell/desktop/kernel_tests.h"

#include "host/kernel.h"

#include <windows.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace xbrt {

namespace {

constexpr uint32_t kReturnAddress = 0x00012345;
constexpr uint32_t kUntouched = 0xEEEEEEEE;
const char kFileText[] = "xbrt kernel test: D:\\ maps to the dir= folder\n";

struct TestSink final : MissingSink {
    std::vector<Missing> seen;
    std::vector<std::string> logged;
    void missing(const Missing &m) noexcept override { seen.push_back(m); }
    void log(std::string_view l) noexcept override { logged.emplace_back(l); }
    [[noreturn]] void return_to_hub(uint32_t reason) noexcept override
    {
        std::fprintf(stderr, "kernel tests: unexpected return to the hub (reason %u)\n", reason);
        std::exit(3);
    }
};

// A guest call as the id stub leaves it: the return address at esp, the arguments above it, a scratch area above
// those for strings and out-parameters.
struct Call {
    GuestMemory &memory;
    uint32_t esp;
    uint32_t scratch;
    GateFrame frame{};
    std::array<uint32_t, 4> ebx_esi_edi_ebp{kUntouched, kUntouched, kUntouched, kUntouched};

    Call(GuestMemory &m, GuestMemory::Stack stack) : memory(m), esp(stack.base.v - 0x1000), scratch(stack.base.v - 0x800)
    {
        std::memset(memory.host<void>({stack.base.v - 0x1000}), 0, 0x1000);
    }
    uint32_t put(const void *bytes, uint32_t n)
    {
        uint32_t at = scratch;
        std::memcpy(memory.host<void>({at}), bytes, n);
        scratch += (n + 15) & ~15u;
        return at;
    }
    uint32_t reserve(uint32_t n)
    {
        std::vector<uint8_t> zero(n);
        return put(zero.data(), n);
    }
    uint32_t &at(uint32_t va) { return *memory.host<uint32_t>({va}); }
    void run(Ordinal o, std::initializer_list<uint32_t> args) { run(o, kUntouched, kUntouched, args); }
    // fastcall: the first two arguments in ecx and edx, the rest pushed.
    void run(Ordinal o, uint32_t ecx, uint32_t edx, std::initializer_list<uint32_t> args)
    {
        at(esp) = kReturnAddress;
        uint32_t i = 0;
        for (uint32_t a : args) at(esp + 4 + 4 * i++) = a;
        auto [ebx, esi, edi, ebp] = ebx_esi_edi_ebp;
        frame = {kUntouched, ecx, edx, ebx, ebp, esi, edi, esp, kReturnAddress, 0x1F80, 0x027F, 0};
        on_host_call(GuestThread::current(), HostCallId::kernel(o), frame);
    }
    // stdcall: back at the return address with the arguments popped.
    std::string returned(uint32_t arg_bytes, uint32_t eax) const
    {
        char buf[160];
        if (frame.eip == kReturnAddress && frame.esp == esp + 4 + arg_bytes && frame.eax == eax) return "";
        snprintf(buf, sizeof buf, "eip=0x%08X esp=+0x%X eax=0x%08X, want eip=0x%08X esp=+0x%X eax=0x%08X", frame.eip,
                 frame.esp - esp, frame.eax, kReturnAddress, 4 + arg_bytes, eax);
        return buf;
    }
};

std::string hex(uint64_t v)
{
    char buf[24];
    snprintf(buf, sizeof buf, "0x%llX", static_cast<unsigned long long>(v));
    return buf;
}

}  // namespace

extern "C" {
void xbox_OhciAttach(void);
uint32_t xbox_OhciRead(uint32_t xbox_va, unsigned size);
void xbox_OhciWrite(uint32_t xbox_va, unsigned size, uint32_t value);
}

namespace {

// The first USB controller as a polling host driver sees it: nxdk's starts the controller with the master, done-head
// and start-of-frame interrupts and never enables root hub status change; it reads the root ports instead.
struct Usb {
    static constexpr uint32_t kBase = 0xFED00000;
    static constexpr uint32_t kControl = 0x04, kCommandStatus = 0x08, kInterruptStatus = 0x0C, kInterruptEnable = 0x10,
                              kHcca = 0x18,
                              kControlHeadEd = 0x20, kRhDescriptorA = 0x48, kRhStatus = 0x50, kRhPortStatus1 = 0x54;
    static constexpr uint32_t kEd = 0x100, kTd = 0x140, kSetup = 0x200, kData = 0x300;

    GuestMemory &memory;
    uint32_t va, pa;

    uint32_t reg(uint32_t off) const { return xbox_OhciRead(kBase + off, 4); }
    void set(uint32_t off, uint32_t v) const { xbox_OhciWrite(kBase + off, 4, v); }
    uint32_t &word(uint32_t off) const { return *memory.host<uint32_t>({va + off}); }
    uint8_t *bytes(uint32_t off) const { return memory.host<uint8_t>({va + off}); }

    template <class Done>
    static bool within_ms(unsigned ms, Done done)
    {
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end) {
            if (done()) return true;
            Sleep(5);
        }
        return done();
    }

    // SETUP, an IN data stage of `in_len` bytes when there is one, then the status stage, at device `address`.
    // Returns "" or what went wrong; the data lands at kData.
    std::string control(uint8_t address, std::initializer_list<uint8_t> setup, uint32_t in_len)
    {
        constexpr uint32_t kNoInterrupt = 7u << 21, kCcNotAccessed = 0xFu << 28, kData0 = 2u << 24, kData1 = 3u << 24;
        constexpr uint32_t kSetupPid = 0u << 19, kOut = 1u << 19, kIn = 2u << 19, kRounding = 1u << 18;
        std::memset(bytes(kEd), 0, 0x200);
        std::memcpy(bytes(kSetup), setup.begin(), 8);
        uint32_t td = kTd;
        auto add = [&](uint32_t flags, uint32_t buffer, uint32_t length) {
            word(td) = flags | kNoInterrupt | kCcNotAccessed;
            word(td + 4) = length ? pa + buffer : 0;
            word(td + 8) = pa + td + 16;
            word(td + 12) = length ? pa + buffer + length - 1 : 0;
            td += 16;
        };
        add(kSetupPid | kData0, kSetup, 8);
        if (in_len) add(kIn | kData1 | kRounding, kData, in_len);
        add((in_len ? kOut : kIn) | kData1, 0, 0);
        uint32_t tail = td;
        word(kEd) = address | (64u << 16);
        word(kEd + 4) = pa + tail;
        word(kEd + 8) = pa + kTd;
        set(kControlHeadEd, pa + kEd);
        set(kControl, reg(kControl) | 0x10);   // ControlListEnable
        set(kCommandStatus, 0x2);              // ControlListFilled
        bool finished = within_ms(1000, [&] { return (word(kEd + 8) & ~0xFu) == pa + tail || (word(kEd + 8) & 1); });
        // The done queue is the driver's until it clears WritebackDoneHead; the controller moves nothing more till then.
        set(kInterruptStatus, 0x2);
        std::string codes;
        for (uint32_t at = kTd; at < tail; at += 16) codes += hex(word(at) >> 28) + " ";
        if (!finished) return "no completion in 1 s (TD codes " + codes + ")";
        for (uint32_t at = kTd; at < tail; at += 16)
            if (word(at) >> 28) return "TD codes " + codes + "(want 0x0 each)";
        return "";
    }
};

// The pad on the root port: the port shows a connected device with its change bit, and through the hub behind it an
// XID device answers with its descriptors (configuration with interface class 0x58). The driver starts the controller
// 31 s after the runtime attaches it, as a title with a long start-up does.
std::string usb_pad_enumerates(GuestMemory &memory)
{
    GuestVa block = memory.allocate_contiguous(0x1000, 0, 0x03FFFFFF, 0x1000, 0x04);
    if (!block.v) return "no contiguous page";
    Usb usb{memory, block.v, memory.physical(block)};
    std::memset(usb.bytes(0), 0, 0x1000);
    xbox_OhciAttach();
    Sleep(31000);
    usb.set(Usb::kHcca, usb.pa);
    usb.set(Usb::kControlHeadEd, 0);
    usb.set(Usb::kControl, 0x80);                                 // UsbOperational
    usb.set(Usb::kInterruptEnable, 0x80000000u | 0x2 | 0x4);       // MIE | WDH | SF, no RHSC
    usb.set(Usb::kRhStatus, 0x10000);                              // LPSC: power the ports

    unsigned ports = usb.reg(Usb::kRhDescriptorA) & 0xFF;
    int port = -1;
    Usb::within_ms(3000, [&] {
        for (unsigned p = 0; p < ports && port < 0; p++)
            if ((usb.reg(Usb::kRhPortStatus1 + 4 * p) & 0x10001) == 0x10001) port = int(p);
        return port >= 0;
    });
    if (port < 0) {
        std::string got;
        for (unsigned p = 0; p < ports; p++) got += " " + hex(usb.reg(Usb::kRhPortStatus1 + 4 * p));
        return "root port status after 3 s:" + got + "; want CCS|CSC (0x10001) on one port";
    }
    uint32_t root = Usb::kRhPortStatus1 + 4 * uint32_t(port);
    usb.set(root, 0x10010);   // clear CSC, SetPortReset
    if (!Usb::within_ms(1000, [&] { return (usb.reg(root) & 0x100002) == 0x100002; }))
        return "root port " + std::to_string(port + 1) + " status " + hex(usb.reg(root)) + " after reset; want PES|PRSC";
    usb.set(root, 0x100000);

    if (std::string e = usb.control(0, {0x80, 6, 0, 1, 0, 0, 18, 0}, 18); !e.empty()) return "hub device descriptor: " + e;
    if (usb.bytes(Usb::kData)[1] != 1 || usb.bytes(Usb::kData)[4] != 9)
        return "the root port's device is not a hub: descriptor type " + hex(usb.bytes(Usb::kData)[1]) + " class " +
               hex(usb.bytes(Usb::kData)[4]);
    if (std::string e = usb.control(0, {0x00, 5, 1, 0, 0, 0, 0, 0}, 0); !e.empty()) return "hub SET_ADDRESS: " + e;
    if (std::string e = usb.control(1, {0x00, 9, 1, 0, 0, 0, 0, 0}, 0); !e.empty()) return "hub SET_CONFIGURATION: " + e;
    int hub_port = 0;
    for (uint8_t p = 1; p <= 4 && !hub_port; p++) {
        usb.control(1, {0x23, 3, 8, 0, p, 0, 0, 0}, 0);   // SetPortFeature(PORT_POWER)
        if (usb.control(1, {0xA3, 0, 0, 0, p, 0, 4, 0}, 4).empty() && (usb.bytes(Usb::kData)[0] & 1)) hub_port = p;
    }
    if (!hub_port) return "no hub port reports a connected device";
    if (std::string e = usb.control(1, {0x23, 3, 4, 0, uint8_t(hub_port), 0, 0, 0}, 0); !e.empty())
        return "hub SetPortFeature(PORT_RESET): " + e;
    bool enabled = Usb::within_ms(1000, [&] {
        return usb.control(1, {0xA3, 0, 0, 0, uint8_t(hub_port), 0, 4, 0}, 4).empty() && (usb.bytes(Usb::kData)[0] & 2);
    });
    if (!enabled) return "hub port " + std::to_string(hub_port) + " not enabled after reset";

    if (std::string e = usb.control(0, {0x80, 6, 0, 1, 0, 0, 18, 0}, 18); !e.empty()) return "pad device descriptor: " + e;
    if (usb.bytes(Usb::kData)[0] != 18 || usb.bytes(Usb::kData)[1] != 1)
        return "pad device descriptor length " + hex(usb.bytes(Usb::kData)[0]) + " type " + hex(usb.bytes(Usb::kData)[1]);
    if (std::string e = usb.control(0, {0x80, 6, 0, 2, 0, 0, 32, 0}, 32); !e.empty()) return "pad configuration: " + e;
    uint8_t interface_class = usb.bytes(Usb::kData)[9 + 5];
    if (interface_class != 0x58) return "pad interface class " + hex(interface_class) + ", want 0x58 (XID)";
    return "";
}

}  // namespace

bool run_kernel_tests(GuestMemory &memory, const std::function<void(const std::string &)> &line)
{
    bool all = true;
    auto result = [&](const char *name, const std::string &failure) {
        line(std::string("kernel ") + name + (failure.empty() ? " ok" : " FAIL " + failure));
        all = all && failure.empty();
    };

    std::filesystem::path disc = std::filesystem::current_path() / "kernel-disc";
    std::filesystem::path saves = std::filesystem::current_path() / "kernel-saves";
    std::filesystem::path home = saves / "home";
    std::filesystem::create_directories(disc);
    std::ofstream(disc / "hello.txt", std::ios::binary) << kFileText;
    const uint32_t file_bytes = uint32_t(std::strlen(kFileText));

    TestSink sink;
    // GuestThread::current() builds the calling thread's ThreadBlock through the installed CPU, as the session does.
    Cpu &cpu = Cpu::install(memory, on_host_call, [](const TrapReport &) noexcept {}, CpuOptions{});
    KernelSetup setup{&memory, &sink, &cpu,
                      std::u16string(reinterpret_cast<const char16_t *>(disc.wstring().c_str())),
                      std::u16string(reinterpret_cast<const char16_t *>(saves.wstring().c_str())),
                      std::u16string(reinterpret_cast<const char16_t *>((home / "saves" / "xbrt").wstring().c_str())),
                      std::u16string(reinterpret_cast<const char16_t *>(home.wstring().c_str())),
                      0,
                      nullptr,
                      {},
                      {},
                      nullptr,
                      nullptr};
    if (Parsed<void> installed = kernel_install(setup); !installed) {
        result("install", installed.error());
        return false;
    }
    GuestMemory::Stack stack = memory.allocate_stack(0x10000);
    if (!stack.base.v) {
        result("stack", "allocate_stack(64 KB) failed");
        return false;
    }

    {
        Call c(memory, stack);
        const char text[] = "kernel test DbgPrint %d";
        c.run({8}, {c.put(text, sizeof text), 7});
        result("DbgPrint", c.returned(0, 0));   // cdecl: the caller pops the arguments
    }
    {
        Call c(memory, stack);
        uint32_t out = c.reserve(8);
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        c.run({128}, {out});
        uint64_t got = *memory.host<uint64_t>({out});
        uint64_t host = (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime;
        std::string failure = c.returned(4, 0);
        if (failure.empty() && (got < host || got - host > 600'000'000ull))
            failure = "time " + hex(got) + " is not within a minute after the host's " + hex(host);
        result("KeQuerySystemTime", failure);
    }

    uint32_t handle = 0;
    {
        Call c(memory, stack);
        const char path[] = "\\??\\D:\\hello.txt";
        uint32_t text = c.put(path, sizeof path);
        uint32_t name = c.reserve(8);
        c.at(name) = uint32_t(sizeof path - 1) | (uint32_t(sizeof path) << 16);   // ANSI_STRING Length, MaximumLength
        c.at(name + 4) = text;
        uint32_t attributes = c.reserve(12);   // OBJECT_ATTRIBUTES: RootDirectory, ObjectName, Attributes
        c.at(attributes + 4) = name;
        c.at(attributes + 8) = 0x40;           // OBJ_CASE_INSENSITIVE
        uint32_t handle_out = c.reserve(4);
        uint32_t iosb = c.reserve(8);
        c.at(iosb) = kUntouched;
        // GENERIC_READ | SYNCHRONIZE, FILE_SHARE_READ, FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE
        c.run({190}, {handle_out, 0x80100000, attributes, iosb, 0, 0x80, 1, 1, 0x60});
        handle = c.at(handle_out);
        std::string failure = c.returned(36, 0);
        if (failure.empty() && (!handle || c.at(iosb) != 0))
            failure = "handle " + hex(handle) + " iosb.status " + hex(c.at(iosb));
        result("NtCreateFile", failure);
    }
    {
        Call c(memory, stack);
        uint32_t iosb = c.reserve(8);
        uint32_t buffer = c.reserve(128);
        uint32_t offset = c.reserve(8);
        c.run({219}, {handle, 0, 0, 0, iosb, buffer, 128, offset});
        std::string failure = c.returned(32, 0);
        std::string got(memory.host<const char>({buffer}), file_bytes);
        if (failure.empty() && (c.at(iosb) != 0 || c.at(iosb + 4) != file_bytes || got != kFileText))
            failure = "iosb " + hex(c.at(iosb)) + "/" + hex(c.at(iosb + 4)) + " read '" + got.substr(0, 24) + "'";
        result("NtReadFile", failure);
    }
    {
        // A stub bridge is refused like a missing export: a stop that names it, the guest's registers untouched.
        Call c(memory, stack);
        sink.seen.clear();
        c.run({10}, {0, 0, 0});
        std::string failure;
        if (c.frame.eip != kReturnAddress || c.frame.esp != c.esp || c.frame.eax != kUntouched)
            failure = "frame changed: eip=" + hex(c.frame.eip) + " esp=" + hex(c.frame.esp) + " eax=" + hex(c.frame.eax);
        else if (sink.seen.size() != 1 || sink.seen[0].kind != MissingKind::KernelExport || !sink.seen[0].stop ||
                 sink.seen[0].what != "Kernel export 0x00A DbgPrompt (10) is a stub in the kernel bridges")
            failure = std::to_string(sink.seen.size()) + " reports, first '" + (sink.seen.empty() ? "" : sink.seen[0].what) + "'";
        result("stub", failure);
    }
    {
        // A reviewed no-op runs its bridge silently and returns.
        Call c(memory, stack);
        sink.seen.clear();
        c.run({47}, {0, 0});
        std::string failure = c.returned(8, 0);
        if (failure.empty() && !sink.seen.empty()) failure = std::to_string(sink.seen.size()) + " reports, first '" + sink.seen[0].what + "'";
        result("noop", failure);
    }
    {
        // A bridge whose layer-A callee does nothing is a stub too: masking an interrupt line is refused, not faked.
        Call c(memory, stack);
        sink.seen.clear();
        c.run({39}, {3});
        std::string failure;
        if (c.frame.eip != kReturnAddress || c.frame.esp != c.esp || c.frame.eax != kUntouched)
            failure = "frame changed: eip=" + hex(c.frame.eip) + " esp=" + hex(c.frame.esp) + " eax=" + hex(c.frame.eax);
        else if (sink.seen.size() != 1 || !sink.seen[0].stop ||
                 sink.seen[0].what != "Kernel export 0x027 HalDisableSystemInterrupt (39) is a stub in the kernel bridges")
            failure = std::to_string(sink.seen.size()) + " reports, first '" + (sink.seen.empty() ? "" : sink.seen[0].what) + "'";
        result("idle-stub", failure);
    }
    {
        // A raise does not return to its caller: the guest resumes in the exception dispatcher, whose arguments are a
        // copy of the record and the CONTEXT of the caller as if RtlRaiseException had returned to it.
        Call c(memory, stack);
        sink.seen.clear();
        uint32_t record = c.reserve(0x50);
        c.at(record) = 0xE06D7363;   // the C++ runtime's exception code
        c.at(record + 0x10) = 1;     // NumberParameters
        c.at(record + 0x14) = 0x19930520;
        c.ebx_esi_edi_ebp = {0x0B0B0B0B, 0x05050505, 0x0D0D0D0D, 0x0E0E0E0E};
        c.run({302}, {record});
        std::string failure;
        uint32_t esp = c.frame.esp;
        uint32_t copy = esp + 12 <= c.esp ? c.at(esp) : 0, context = esp + 12 <= c.esp ? c.at(esp + 4) : 0;
        if (c.frame.eip == kReturnAddress || !copy || !context || !sink.seen.empty())
            failure = "eip=" + hex(c.frame.eip) + " esp=" + hex(esp) + ", " + std::to_string(sink.seen.size()) + " reports";
        else if (c.at(copy) != 0xE06D7363 || c.at(copy + 0x0C) != kReturnAddress || c.at(copy + 0x10) != 1 ||
                 c.at(copy + 0x14) != 0x19930520)
            failure = "record copy code " + hex(c.at(copy)) + " address " + hex(c.at(copy + 0x0C)) + " parameter " +
                      hex(c.at(copy + 0x14));
        else if (c.at(context + 0x224) != kReturnAddress || c.at(context + 0x230) != c.esp + 8 ||
                 c.at(context + 0x210) != 0x0B0B0B0B || c.at(context + 0x20C) != 0x05050505 ||
                 c.at(context + 0x208) != 0x0D0D0D0D || c.at(context + 0x220) != 0x0E0E0E0E || (context + 4) % 16)
            failure = "context Eip " + hex(c.at(context + 0x224)) + " Esp " + hex(c.at(context + 0x230)) + " Ebx " +
                      hex(c.at(context + 0x210)) + " at " + hex(context);
        result("raise-dispatch", failure);
    }
    {
        // No bridge: a stop that names the export and the call site, and nothing written to the guest's registers.
        Call c(memory, stack);
        sink.seen.clear();
        c.run({370}, {});
        std::string failure;
        if (c.frame.eip != kReturnAddress || c.frame.esp != c.esp || c.frame.eax != kUntouched)
            failure = "frame changed: eip=" + hex(c.frame.eip) + " esp=" + hex(c.frame.esp) + " eax=" + hex(c.frame.eax);
        else if (sink.seen.size() != 1 || sink.seen[0].kind != MissingKind::KernelExport || !sink.seen[0].stop ||
                 sink.seen[0].what != "Missing kernel export 0x172 (370)" || sink.seen[0].where.v != kReturnAddress ||
                 sink.seen[0].where_symbol != "0x00012345")
            failure = std::to_string(sink.seen.size()) + " reports, first '" + (sink.seen.empty() ? "" : sink.seen[0].what) +
                      "' at '" + (sink.seen.empty() ? "" : sink.seen[0].where_symbol) + "'";
        result("unbridged", failure);
    }
    {
        // Under home= the runtime writes only in saves\xbrt and cache\xbrt: a guest path that climbs out of its TDATA
        // folder is refused and leaves nothing behind.
        auto create = [&](const char *path) {
            Call c(memory, stack);
            uint32_t text = c.put(path, uint32_t(std::strlen(path) + 1));
            uint32_t name = c.reserve(8);
            c.at(name) = uint32_t(std::strlen(path)) | (uint32_t(std::strlen(path) + 1) << 16);
            c.at(name + 4) = text;
            uint32_t attributes = c.reserve(12);
            c.at(attributes + 4) = name;
            c.at(attributes + 8) = 0x40;
            uint32_t handle_out = c.reserve(4);
            uint32_t iosb = c.reserve(8);
            // GENERIC_WRITE | SYNCHRONIZE, FILE_OVERWRITE_IF, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE
            c.run({190}, {handle_out, 0x40100000, attributes, iosb, 0, 0x80, 0, 5, 0x60});
            uint32_t status = c.frame.eax;
            if (status == 0) {
                Call close(memory, stack);
                close.run({187}, {c.at(handle_out)});
            }
            return status;
        };
        uint32_t inside = create("\\Device\\Harddisk0\\Partition1\\TDATA\\00000000\\inside.txt");
        uint32_t outside = create("\\Device\\Harddisk0\\Partition1\\TDATA\\00000000\\..\\..\\..\\..\\outside.txt");
        bool inside_written = std::filesystem::exists(home / "saves" / "xbrt" / "00000000" / "TDATA" / "inside.txt");
        bool outside_written = std::filesystem::exists(home / "outside.txt");
        std::string failure;
        if (inside != 0 || !inside_written || outside == 0 || outside_written)
            failure = "inside " + hex(inside) + (inside_written ? " written" : " not written") + ", outside " + hex(outside) +
                      (outside_written ? " written to home\\outside.txt" : " not written") +
                      "; want inside 0x0 written, outside refused and not written";
        result("home-writes", failure);
    }
    // Argument layouts from nxdk's xboxkrnl.h: each call pops what the Xbox kernel's own `ret N` pops and reads its
    // arguments where the caller put them.
    {
        Call c(memory, stack);
        uint32_t addend = c.reserve(8);
        c.at(addend) = 0xFFFFFFFF;
        c.at(addend + 4) = 1;
        // Addend, Increment (LARGE_INTEGER by value: two slots), Lock
        c.run({19}, {addend, 1, 1, c.reserve(4)});
        std::string failure = c.returned(16, 0xFFFFFFFF);
        if (failure.empty() && (c.frame.edx != 1 || c.at(addend) != 0 || c.at(addend + 4) != 3))
            failure = "old high " + hex(c.frame.edx) + ", addend " + hex(c.at(addend + 4)) + ":" + hex(c.at(addend)) +
                      "; want old high 0x1, addend 0x3:0x0";
        result("ExInterlockedAddLargeInteger", failure);
    }
    {
        Call c(memory, stack);
        uint32_t dest = c.reserve(8), exchange = c.reserve(8), comparand = c.reserve(8);
        c.at(dest) = 0x11111111;
        c.at(dest + 4) = 0x22222222;
        c.at(exchange) = 0x33333333;
        c.at(exchange + 4) = 0x44444444;
        c.at(comparand) = 0x11111111;
        c.at(comparand + 4) = 0x22222222;
        c.run({21}, dest, exchange, {comparand});
        std::string failure = c.returned(4, 0x11111111);
        if (failure.empty() && (c.frame.edx != 0x22222222 || c.at(dest) != 0x33333333 || c.at(dest + 4) != 0x44444444))
            failure = "old high " + hex(c.frame.edx) + ", destination " + hex(c.at(dest + 4)) + ":" + hex(c.at(dest)) +
                      "; want old high 0x22222222, destination 0x44444444:0x33333333";
        result("ExInterlockedCompareExchange64", failure);
    }
    {
        Call c(memory, stack);
        uint32_t dest = c.reserve(4);
        c.at(dest) = 5;
        c.run({51}, dest, 9, {5});   // eax holds kUntouched, which is no argument
        std::string failure = c.returned(4, 5);
        if (failure.empty() && c.at(dest) != 9) failure = "destination " + hex(c.at(dest)) + ", want 0x9";
        result("InterlockedCompareExchange", failure);
    }
    {
        Call c(memory, stack);
        c.run({307}, 0x11223344, kUntouched, {});
        result("RtlUlongByteSwap", c.returned(0, 0x44332211));
    }
    {
        // x / 10000 by the magic multiplier 2^77 / 10000 and a shift of 13, both signs.
        const uint64_t magic = 0xD1B71758E219652Cull;
        const int64_t dividend = 123456789ll * 10000 + 1234;
        std::string failure;
        for (int64_t x : {dividend, -dividend}) {
            Call c(memory, stack);
            c.run({283}, {uint32_t(x), uint32_t(uint64_t(x) >> 32), uint32_t(magic), uint32_t(magic >> 32), 13});
            int64_t want = x / 10000;
            failure = c.returned(20, uint32_t(want));
            if (failure.empty() && c.frame.edx != uint32_t(uint64_t(want) >> 32))
                failure = "quotient high " + hex(c.frame.edx) + ", want " + hex(uint32_t(uint64_t(want) >> 32));
            if (!failure.empty()) break;
        }
        result("RtlExtendedMagicDivide", failure);
    }
    {
        Call c(memory, stack);
        uint32_t head = c.reserve(8), first = c.reserve(8), second = c.reserve(8);
        c.at(head) = head;
        c.at(head + 4) = head;
        c.run({33}, head, first, {});
        std::string failure = c.returned(0, 0);
        if (failure.empty()) {
            c.run({33}, head, second, {});
            failure = c.returned(0, first);
        }
        if (failure.empty() && (c.at(head) != first || c.at(head + 4) != second || c.at(first) != second ||
                                c.at(second + 4) != first || c.at(second) != head))
            failure = "list head " + hex(c.at(head)) + "/" + hex(c.at(head + 4)) + " is not head -> first -> second";
        result("ExfInterlockedInsertTailList", failure);
    }
    {
        // The clocks a title reads as data keep time: KeTickCount in milliseconds, KeInterruptTime and KeSystemTime
        // (KSYSTEM_TIME: LowPart, High1Time, High2Time) in 100 ns units.
        auto read = [&](uint16_t ordinal) {
            GuestVa va = kernel_data_export({ordinal});
            const uint32_t *p = memory.host<const uint32_t>(va);
            return ordinal == 156 ? uint64_t(p[0]) : (uint64_t(p[1]) << 32) | p[0];
        };
        uint64_t tick = read(156), interrupt = read(120), system = read(154);
        Sleep(100);
        uint64_t tick_ms = read(156) - tick, interrupt_ms = (read(120) - interrupt) / 10000,
                 system_ms = (read(154) - system) / 10000;
        std::string failure;
        const std::pair<const char *, uint64_t> moved[] = {
            {"KeTickCount", tick_ms}, {"KeInterruptTime", interrupt_ms}, {"KeSystemTime", system_ms}};
        for (auto [name, ms] : moved)
            if (ms < 50 || ms > 1000)
                failure += std::string(failure.empty() ? "" : ", ") + name + " moved " + std::to_string(ms) + " ms";
        if (!failure.empty()) failure += " across a 100 ms sleep";
        result("clocks", failure);
    }
    result("usb-pad", usb_pad_enumerates(memory));
    return all;
}

}  // namespace xbrt
