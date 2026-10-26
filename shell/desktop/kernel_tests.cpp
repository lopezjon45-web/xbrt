// xbrt_desktop.exe --kernel-tests: host/kernel.cpp's on_host_call against the fork's bridges, through hand-built
// GateFrames on a guest stack in system space. No guest code runs. One line per test, "kernel <name> ok|FAIL <detail>".
#include "shell/desktop/kernel_tests.h"

#include "host/kernel.h"

#include <windows.h>

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
    KernelSetup setup{&memory, &sink, nullptr,
                      std::u16string(reinterpret_cast<const char16_t *>(disc.wstring().c_str())),
                      std::u16string(reinterpret_cast<const char16_t *>(saves.wstring().c_str())),
                      {}, {}, nullptr, nullptr};
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
        // A stub bridge runs and warns once per ordinal; the guest continues.
        Call c(memory, stack);
        sink.seen.clear();
        c.run({10}, {0, 0});
        std::string failure = c.returned(8, 0);
        c.run({10}, {0, 0});
        if (failure.empty()) failure = c.returned(8, 0);
        if (failure.empty() && (sink.seen.size() != 1 || sink.seen[0].kind != MissingKind::KernelStub ||
                                sink.seen[0].stop || sink.seen[0].what != "stub: 0x00A DbgPrompt (10)"))
            failure = std::to_string(sink.seen.size()) + " reports, first '" + (sink.seen.empty() ? "" : sink.seen[0].what) + "'";
        result("stub", failure);
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
    return all;
}

}  // namespace xbrt
