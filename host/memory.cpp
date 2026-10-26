#include "host/memory.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>

namespace xbrt {

namespace {

constexpr uint32_t kUserBase = 0x00010000;
constexpr uint32_t kContiguousBase = 0x80000000;
constexpr uint32_t kContiguousSize = 0x10000000;
constexpr uint32_t kRuntimeBase = 0x90000000;
constexpr uint32_t kRuntimeSize = 0x01000000;
constexpr uint32_t kPageTablesBase = 0xC0000000;
constexpr uint32_t kPageTablesSize = 0x00400000;
constexpr uint32_t kSystemBase = 0xD0000000;
constexpr uint32_t kSystemSize = 0x20000000;
constexpr uint32_t kTiledBase = 0xF0000000;
constexpr uint32_t kTiledSize = 0x0FC00000;
constexpr uint32_t kFlashBase = 0xFFC00000;
constexpr uint32_t kFlashSize = 0x003F0000;
constexpr uint32_t kPage = 0x1000;
constexpr uint32_t kStackMin = 0x10000;

// The ranges that take a view of the RAM section are placeholders, which MapViewOfFile3 can replace. The others are
// plain reservations: a placeholder cannot be committed page by page, and the runtime region and stacks commit in
// 4 KB and 64 KB pieces.
struct Range {
    const char *name;
    uint32_t base, size;
    bool placeholder;
};

struct View {
    const char *name;
    uint32_t va, offset, size;
};

void *ptr(uint32_t va) { return reinterpret_cast<void *>(static_cast<uintptr_t>(va)); }

std::string format(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return buf;
}

}  // namespace

GuestMemory::~GuestMemory()
{
    if (section_) CloseHandle(section_);
}

Parsed<std::unique_ptr<GuestMemory>> GuestMemory::reserve(RamSize ram)
{
    std::unique_ptr<GuestMemory> m(new GuestMemory);
    const uint32_t bytes = static_cast<uint32_t>(ram);
    m->ram_ = bytes;
    HANDLE process = GetCurrentProcess();
    auto fail = [&m](const std::string &step) {
        m->log_.push_back("reserve FAIL " + step);
        std::string all;
        for (const std::string &l : m->log_) all += (all.empty() ? "" : "\n") + l;
        return std::unexpected(all);
    };

    const Range ranges[] = {
        {"user", kUserBase, bytes - kUserBase, true},
        {"contiguous", kContiguousBase, kContiguousSize, true},
        {"runtime", kRuntimeBase, kRuntimeSize, false},
        {"pagetables", kPageTablesBase, kPageTablesSize, false},
        {"system", kSystemBase, kSystemSize, false},
        {"tiled", kTiledBase, kTiledSize, true},
        {"flash", kFlashBase, kFlashSize, false},
    };
    for (const Range &r : ranges) {
        ULONG type = r.placeholder ? MEM_RESERVE | MEM_RESERVE_PLACEHOLDER : MEM_RESERVE;
        void *got = VirtualAlloc2(process, ptr(r.base), r.size, type, PAGE_NOACCESS, nullptr, 0);
        DWORD err = got ? 0 : GetLastError();
        std::string line = format("  %s %s 0x%08X+0x%08X %s err=%lu", r.placeholder ? "placeholder" : "reserve", r.name,
                                  r.base, r.size, got == ptr(r.base) ? "ok" : "FAIL", static_cast<unsigned long>(err));
        m->log_.push_back(line);
        if (got != ptr(r.base)) return fail(format("%s 0x%08X+0x%08X err=%lu", r.name, r.base, r.size, static_cast<unsigned long>(err)));
    }

    bool rwx = true;
    m->section_ = CreateFileMappingFromApp(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE, bytes, nullptr);
    if (!m->section_) {
        DWORD err = GetLastError();
        rwx = false;
        m->section_ = CreateFileMappingFromApp(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, bytes, nullptr);
        m->log_.push_back(format("  section 0x%08X rwx refused err=%lu, rw %s err=%lu", bytes, static_cast<unsigned long>(err),
                                 m->section_ ? "ok" : "FAIL", static_cast<unsigned long>(m->section_ ? 0 : GetLastError())));
        if (!m->section_) return fail(format("section 0x%08X err=%lu", bytes, static_cast<unsigned long>(GetLastError())));
    } else {
        m->log_.push_back(format("  section 0x%08X rwx ok", bytes));
    }

    const View views[] = {
        {"user", kUserBase, kUserBase, bytes - kUserBase},
        {"contiguous", kContiguousBase, 0, bytes},
        {"tiled", kTiledBase, 0, bytes},
    };
    for (const View &v : views) {
        // The contiguous and tiled placeholders are larger than RAM: split the view's part off first.
        bool exact = (v.va == kUserBase);
        if (!exact && !VirtualFree(ptr(v.va), v.size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
            DWORD err = GetLastError();
            m->log_.push_back(format("  split %s 0x%08X+0x%08X FAIL err=%lu", v.name, v.va, v.size, static_cast<unsigned long>(err)));
            return fail(format("split %s 0x%08X+0x%08X err=%lu", v.name, v.va, v.size, static_cast<unsigned long>(err)));
        }
        // A view's mapping protection caps every later VirtualProtect on it, so guest code pages need an RWX view.
        // MapViewOfFile3FromApp refuses execute protections (error 87 on windows-2022); MapViewOfFile3 does not.
        void *got = nullptr;
        if (rwx) {
            got = MapViewOfFile3(m->section_, process, ptr(v.va), v.offset, v.size, MEM_REPLACE_PLACEHOLDER,
                                 PAGE_EXECUTE_READWRITE, nullptr, 0);
            if (got != ptr(v.va)) {
                m->log_.push_back(format("  view %s rwx refused err=%lu", v.name, static_cast<unsigned long>(GetLastError())));
                rwx = false;
            }
        }
        if (!rwx)
            got = MapViewOfFile3FromApp(m->section_, process, ptr(v.va), v.offset, v.size, MEM_REPLACE_PLACEHOLDER,
                                        PAGE_READWRITE, nullptr, 0);
        DWORD err = got ? 0 : GetLastError();
        m->log_.push_back(format("  view %s 0x%08X+0x%08X offset 0x%08X %s %s err=%lu", v.name, v.va, v.size, v.offset,
                                 rwx ? "rwx" : "rw", got == ptr(v.va) ? "ok" : "FAIL", static_cast<unsigned long>(err)));
        if (got != ptr(v.va)) return fail(format("view %s 0x%08X+0x%08X err=%lu", v.name, v.va, v.size, static_cast<unsigned long>(err)));
    }
    m->log_.push_back(format("reserve ok ram=%uMB views=%s", bytes >> 20, rwx ? "rwx" : "rw"));
    return m;
}

std::string GuestMemory::identity_check()
{
    constexpr uint32_t user = 0x00100000;
    volatile uint32_t *u = host<volatile uint32_t>({user});
    volatile uint32_t *c = host<volatile uint32_t>({kContiguousBase + user});
    volatile uint32_t *t = host<volatile uint32_t>({kTiledBase + user});
    const uint32_t pattern = 0x5A3C0FF1;
    *u = pattern;
    if (*c != pattern) return format("identity FAIL contiguous 0x%08X read 0x%08X want 0x%08X", kContiguousBase + user, *c, pattern);
    if (*t != pattern) return format("identity FAIL tiled 0x%08X read 0x%08X want 0x%08X", kTiledBase + user, *t, pattern);
    *t = ~pattern;
    if (*u != ~pattern) return format("identity FAIL user 0x%08X read 0x%08X want 0x%08X after tiled write", user, *u, ~pattern);
    ULONG old = 0;
    if (!VirtualProtectFromApp(ptr(user), kPage, PAGE_EXECUTE_READWRITE, &old))
        return format("identity FAIL rwx 0x%08X err=%lu", user, static_cast<unsigned long>(GetLastError()));
    ULONG before = old;
    if (!VirtualProtectFromApp(ptr(user), kPage, before, &old))
        return format("identity FAIL rw 0x%08X err=%lu", user, static_cast<unsigned long>(GetLastError()));
    return format("identity ok 0x%08X=0x%08X=0x%08X rwx", user, kContiguousBase + user, kTiledBase + user);
}

GuestMemory::Stack GuestMemory::allocate_stack(uint32_t size)
{
    size = (size < kStackMin ? kStackMin : size + kPage - 1) & ~(kPage - 1);
    uint32_t limit = kSystemBase + stack_cursor_;
    if (stack_cursor_ + size > kSystemSize) return {};
    void *got = VirtualAlloc2(GetCurrentProcess(), ptr(limit), size, MEM_COMMIT, PAGE_READWRITE, nullptr, 0);
    if (got != ptr(limit)) return {};
    stack_cursor_ += size + kPage;
    return {{limit + size}, {limit}};
}

GuestVa GuestMemory::runtime_alloc(uint32_t size, uint32_t alignment, bool exec)
{
    if (exec && alignment < kPage) alignment = kPage;
    if (alignment == 0) alignment = 1;
    uint32_t start = (runtime_cursor_ + alignment - 1) & ~(alignment - 1);
    uint32_t end = start + size;
    if (exec) end = (end + kPage - 1) & ~(kPage - 1);
    if (end > kRuntimeSize) return {};
    uint32_t committed = (runtime_cursor_ + kPage - 1) & ~(kPage - 1);
    uint32_t commit_end = (end + kPage - 1) & ~(kPage - 1);
    if (commit_end > committed) {
        void *want = ptr(kRuntimeBase + committed);
        if (VirtualAlloc2(GetCurrentProcess(), want, commit_end - committed, MEM_COMMIT, PAGE_READWRITE, nullptr, 0) != want)
            return {};
    }
    if (exec) {
        ULONG old = 0;
        void *page = ptr(kRuntimeBase + start);
        if (!VirtualProtectFromApp(page, end - start, PAGE_EXECUTE_READWRITE, &old)) return {};
        FlushInstructionCache(GetCurrentProcess(), page, end - start);
    }
    runtime_cursor_ = end;
    return {kRuntimeBase + start};
}

}  // namespace xbrt
