// xbrt_desktop.exe --kernel-tests: host/kernel.cpp's on_host_call against the fork's bridges, through hand-built
// GateFrames on a guest stack in system space. No guest code runs. One line per test, "kernel <name> ok|FAIL <detail>".
#include "shell/desktop/kernel_tests.h"

#include "host/kernel.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
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
    void run(Ordinal o, std::initializer_list<uint32_t> args)
    {
        at(esp) = kReturnAddress;
        uint32_t i = 0;
        for (uint32_t a : args) at(esp + 4 + 4 * i++) = a;
        frame = {kUntouched, kUntouched, kUntouched, kUntouched, kUntouched, kUntouched, kUntouched, esp, kReturnAddress, 0x1F80,
                 0x027F, 0};
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
// XID device answers with its descriptors (configuration with interface class 0x58).
std::string usb_pad_enumerates(GuestMemory &memory)
{
    GuestVa block = memory.allocate_contiguous(0x1000, 0, 0x03FFFFFF, 0x1000, 0x04);
    if (!block.v) return "no contiguous page";
    Usb usb{memory, block.v, memory.physical(block)};
    std::memset(usb.bytes(0), 0, 0x1000);
    xbox_OhciAttach();
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
    std::filesystem::create_directories(disc);
    std::ofstream(disc / "hello.txt", std::ios::binary) << kFileText;
    const uint32_t file_bytes = uint32_t(std::strlen(kFileText));

    TestSink sink;
    // GuestThread::current() builds the calling thread's ThreadBlock through the installed CPU, as the session does.
    Cpu &cpu = Cpu::install(memory, on_host_call, [](const TrapReport &) noexcept {}, CpuOptions{});
    KernelSetup setup{&memory, &sink, &cpu,
                      std::u16string(reinterpret_cast<const char16_t *>(disc.wstring().c_str())),
                      std::u16string(reinterpret_cast<const char16_t *>(saves.wstring().c_str())),
                      std::u16string(reinterpret_cast<const char16_t *>((saves / "saves").wstring().c_str())),
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
    result("usb-pad", usb_pad_enumerates(memory));
    return all;
}

}  // namespace xbrt
