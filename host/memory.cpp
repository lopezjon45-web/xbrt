#include "host/memory.h"

#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

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
    m->pfn_.assign(bytes / kPage, Owner::Free);
    m->user_protect_.assign(bytes / kPage, 0);
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

namespace {

constexpr uint32_t kReservationGranularity = 0x10000;
constexpr uint32_t kProtectFlags = xbox_page::Guard | xbox_page::NoCache | xbox_page::WriteCombine;

uint32_t pages_spanned(uint32_t base, uint32_t size)
{
    uint64_t end = uint64_t(base) + size;
    return uint32_t(((end + kPage - 1) & ~uint64_t(kPage - 1)) - (base & ~uint32_t(kPage - 1))) / kPage;
}

uint32_t round_up(uint32_t v, uint32_t to) { return (v + to - 1) & ~(to - 1); }

bool valid_protect(uint32_t p)
{
    uint32_t access = p & ~kProtectFlags;
    switch (access) {
    case xbox_page::NoAccess:
    case xbox_page::ReadOnly:
    case xbox_page::ReadWrite:
    case xbox_page::Execute:
    case xbox_page::ExecuteRead:
    case xbox_page::ExecuteReadWrite: break;
    default: return false;
    }
    // Guard, NoCache and WriteCombine make no sense on a no-access page, and NoCache and WriteCombine exclude each other.
    if (access == xbox_page::NoAccess && (p & kProtectFlags)) return false;
    return (p & (xbox_page::NoCache | xbox_page::WriteCombine)) != (xbox_page::NoCache | xbox_page::WriteCombine);
}

// The Xbox and Win32 access values coincide; the cache flags do not apply to a section view.
DWORD host_protect(uint32_t p) { return p & ~kProtectFlags; }

bool protect_pages(uint32_t va, uint32_t size, DWORD protect)
{
    ULONG old = 0;
    return VirtualProtectFromApp(ptr(va), size, protect, &old) != 0;
}

}  // namespace

bool GuestMemory::user_range(uint32_t base, uint32_t size) const
{
    return base >= kUserBase && base < ram_ && size <= ram_ - base;
}

// A free run of `pages` user pages at a 64 KB boundary, as the Xbox places reservations; 0 when there is none.
uint32_t GuestMemory::find_free_user(uint32_t pages, bool top_down) const
{
    const uint32_t step = kReservationGranularity / kPage;
    const uint32_t first = kUserBase / kPage, total = ram_ / kPage;
    auto free_run = [this](uint32_t at, uint32_t n) {
        for (uint32_t i = 0; i < n; i++)
            if (pfn_[at + i] != Owner::Free) return false;
        return true;
    };
    if (pages > total - first) return 0;
    if (top_down) {
        for (uint32_t at = (total - pages) / step * step; at >= first; at -= step) {
            if (free_run(at, pages)) return at * kPage;
            if (at < step) break;
        }
        return 0;
    }
    for (uint32_t at = first; at + pages <= total; at += step)
        if (free_run(at, pages)) return at * kPage;
    return 0;
}

// The reservation holding [base, base+size); end() when base is free. *overflow says the range runs past it.
std::map<uint32_t, GuestMemory::Vad>::const_iterator GuestMemory::vad_containing(uint32_t base, uint32_t size,
                                                                                  bool *overflow) const
{
    *overflow = false;
    auto it = vads_.upper_bound(base);
    if (it == vads_.begin()) return vads_.end();
    --it;
    if (base >= it->first + it->second.size) return vads_.end();
    *overflow = uint64_t(base) + size > uint64_t(it->first) + it->second.size;
    return it;
}

void GuestMemory::commit_user(uint32_t va, uint32_t protect, bool image)
{
    uint32_t page = va / kPage;
    if (user_protect_[page] == 0) {
        pfn_[page] = image ? Owner::Image : Owner::User;
        protect_pages(va, kPage, PAGE_READWRITE);
        memset(ptr(va), 0, kPage);
    }
    user_protect_[page] = uint16_t(protect);
    protect_pages(va, kPage, host_protect(protect));
}

void GuestMemory::decommit_user(uint32_t va, Owner to)
{
    uint32_t page = va / kPage;
    if (user_protect_[page]) protect_pages(va, kPage, PAGE_NOACCESS);
    user_protect_[page] = 0;
    pfn_[page] = to;
}

uint32_t GuestMemory::allocate_virtual(GuestVa *base_io, uint32_t *size_io, uint32_t type, uint32_t protect)
{
    using namespace xbox_mem;
    uint32_t base = base_io->v, size = *size_io;
    if (size == 0) return xbox_status::InvalidParameter;
    if (type & ~(Commit | Reserve | TopDown | Reset | NoZero)) return xbox_status::InvalidParameter;
    if ((type & Reset) && type != Reset) return xbox_status::InvalidParameter;
    if ((type & (Commit | Reserve | Reset)) == 0) return xbox_status::InvalidParameter;
    if (!valid_protect(protect)) return xbox_status::InvalidPageProtection;
    if (base > 0x7FFEFFFF || uint64_t(base) + size > 0x7FFF0000) return xbox_status::InvalidParameter;

    std::lock_guard<std::mutex> hold(lock_);
    uint32_t vad_base = 0, vad_size = 0;
    bool made_vad = false;
    if ((type & Reserve) || base == 0) {
        vad_size = round_up(size, kPage);
        if (base == 0) {
            vad_base = find_free_user(vad_size / kPage, (type & TopDown) != 0);
            if (!vad_base) return xbox_status::NoMemory;
        } else {
            vad_base = base & ~(kReservationGranularity - 1);
            if (!user_range(vad_base, vad_size)) return xbox_status::ConflictingAddresses;
            for (uint32_t p = vad_base / kPage; p < (vad_base + vad_size) / kPage; p++)
                if (pfn_[p] != Owner::Free) return xbox_status::ConflictingAddresses;
        }
        for (uint32_t p = vad_base / kPage; p < (vad_base + vad_size) / kPage; p++) pfn_[p] = Owner::Reserved;
        vads_[vad_base] = {vad_size, protect};
        made_vad = true;
        if ((type & Commit) == 0) {
            *base_io = {vad_base};
            *size_io = vad_size;
            return xbox_status::Success;
        }
        base = vad_base;
        size = vad_size;
    }

    uint32_t first = base & ~(kPage - 1), bytes = pages_spanned(base, size) * kPage;
    bool overflow = false;
    auto vad = vad_containing(first, bytes, &overflow);
    if (vad == vads_.end() || overflow) {
        if (made_vad) {
            for (uint32_t p = vad_base / kPage; p < (vad_base + vad_size) / kPage; p++) pfn_[p] = Owner::Free;
            vads_.erase(vad_base);
        }
        return xbox_status::ConflictingAddresses;
    }
    if (type == Reset) {
        *base_io = {first};
        *size_io = bytes;
        return xbox_status::Success;
    }
    for (uint32_t va = first; va < first + bytes; va += kPage) commit_user(va, protect, false);
    *base_io = {first};
    *size_io = bytes;
    return xbox_status::Success;
}

uint32_t GuestMemory::free_virtual(GuestVa *base_io, uint32_t *size_io, uint32_t type)
{
    using namespace xbox_mem;
    uint32_t base = base_io->v, size = *size_io;
    if (type != Decommit && type != Release) return xbox_status::InvalidParameter;
    if (base > 0x7FFEFFFF || uint64_t(base) + size > 0x7FFF0000) return xbox_status::InvalidParameter;
    uint32_t first = base & ~(kPage - 1), bytes = pages_spanned(base, size) * kPage;

    std::lock_guard<std::mutex> hold(lock_);
    bool overflow = false;
    auto vad = vad_containing(first, bytes, &overflow);
    if (vad == vads_.end()) return xbox_status::MemoryNotAllocated;
    if (overflow) return xbox_status::UnableToFreeVm;
    uint32_t vad_base = vad->first, vad_end = vad_base + vad->second.size, vad_protect = vad->second.protect;
    if (bytes == 0) {
        if (type == Release && first != vad_base) return xbox_status::FreeVmNotAtBase;
        if (first == vad_base) bytes = vad_end - vad_base;
    }
    if (type == Release) {
        for (uint32_t va = first; va < first + bytes; va += kPage) decommit_user(va, Owner::Free);
        // The remainder of a partly released reservation stays reserved on both sides of the hole.
        vads_.erase(vad_base);
        if (first > vad_base) vads_[vad_base] = {first - vad_base, vad_protect};
        if (first + bytes < vad_end) vads_[first + bytes] = {vad_end - (first + bytes), vad_protect};
    } else {
        for (uint32_t va = first; va < first + bytes; va += kPage) decommit_user(va, Owner::Reserved);
    }
    *base_io = {first};
    *size_io = bytes;
    return xbox_status::Success;
}

uint32_t GuestMemory::protect_virtual(GuestVa *base_io, uint32_t *size_io, uint32_t protect, uint32_t *old_protect)
{
    uint32_t base = base_io->v, size = *size_io;
    if (size == 0) return xbox_status::InvalidParameter;
    if (base > 0x7FFEFFFF || uint64_t(base) + size > 0x7FFF0000) return xbox_status::InvalidParameter;
    if (!valid_protect(protect)) return xbox_status::InvalidPageProtection;
    uint32_t first = base & ~(kPage - 1), bytes = pages_spanned(base, size) * kPage;

    std::lock_guard<std::mutex> hold(lock_);
    bool overflow = false;
    auto vad = vad_containing(first, bytes, &overflow);
    if (vad == vads_.end() || overflow) return xbox_status::ConflictingAddresses;
    for (uint32_t va = first; va < first + bytes; va += kPage)
        if (user_protect_[va / kPage] == 0) return xbox_status::NotCommitted;
    *old_protect = user_protect_[first / kPage];
    for (uint32_t va = first; va < first + bytes; va += kPage) commit_user(va, protect, pfn_[va / kPage] == Owner::Image);
    *base_io = {first};
    *size_io = bytes;
    return xbox_status::Success;
}

uint32_t GuestMemory::query_virtual(GuestVa va, XboxMemoryBasicInformation *out) const
{
    if (va.v > 0x7FFEFFFF) return xbox_status::InvalidParameter;
    uint32_t page = va.v & ~(kPage - 1);
    std::lock_guard<std::mutex> hold(lock_);
    bool overflow = false;
    auto vad = vad_containing(page, kPage, &overflow);
    if (vad == vads_.end()) {
        auto next = vads_.upper_bound(page);
        uint32_t end = next == vads_.end() ? 0x7FFF0000 : next->first;
        if (page < kUserBase) end = kUserBase;
        *out = {page, 0, 0, end - page, xbox_mem::Free, xbox_page::NoAccess, 0};
        return xbox_status::Success;
    }
    uint16_t protect = user_protect_[page / kPage];
    uint32_t end = vad->first + vad->second.size, run = page;
    while (run < end && user_protect_[run / kPage] == protect) run += kPage;
    *out = {page, vad->first, vad->second.protect, run - page, protect ? xbox_mem::Commit : xbox_mem::Reserve, protect,
            xbox_mem::Private};
    return xbox_status::Success;
}

GuestVa GuestMemory::allocate_contiguous(uint32_t size, uint32_t lowest, uint32_t highest, uint32_t alignment, uint32_t protect)
{
    if (size == 0 || !valid_protect(protect)) return {};
    if (alignment < kPage) alignment = kPage;
    if (alignment & (alignment - 1)) return {};
    uint32_t pages = round_up(size, kPage) / kPage;
    uint32_t top = std::min<uint64_t>(uint64_t(highest) + 1, ram_) & ~uint64_t(kPage - 1);
    std::lock_guard<std::mutex> hold(lock_);
    if (top < pages * kPage) return {};
    // Top down, as the Xbox kernel places contiguous memory, so it stays clear of the image and the user heap.
    for (uint32_t pa = (top - pages * kPage) & ~(alignment - 1); pa >= lowest; pa -= alignment) {
        bool free = true;
        for (uint32_t p = pa / kPage; p < pa / kPage + pages && free; p++) free = pfn_[p] == Owner::Free;
        if (free) {
            for (uint32_t p = pa / kPage; p < pa / kPage + pages; p++) pfn_[p] = Owner::Contiguous;
            contiguous_[kContiguousBase + pa] = {pages * kPage, protect};
            protect_pages(kContiguousBase + pa, pages * kPage, PAGE_READWRITE);
            memset(ptr(kContiguousBase + pa), 0, pages * kPage);
            protect_pages(kContiguousBase + pa, pages * kPage, host_protect(protect));
            return {kContiguousBase + pa};
        }
        if (pa < alignment) break;
    }
    return {};
}

void GuestMemory::free_contiguous(GuestVa va)
{
    std::lock_guard<std::mutex> hold(lock_);
    auto it = contiguous_.find(va.v);
    if (it == contiguous_.end()) return;
    uint32_t pa = va.v - kContiguousBase;
    for (uint32_t p = pa / kPage; p < (pa + it->second.size) / kPage; p++) pfn_[p] = Owner::Free;
    protect_pages(va.v, it->second.size, PAGE_NOACCESS);
    contiguous_.erase(it);
}

// System space is private host memory, so its blocks come from a cursor and an exact-size free list; a block's VA
// is never reused for another size.
uint32_t GuestMemory::system_block(uint32_t size, uint32_t protect, uint32_t *counter)
{
    uint32_t va = 0;
    auto reuse = system_free_.find(size);
    if (reuse != system_free_.end()) {
        va = reuse->second;
        system_free_.erase(reuse);
    } else {
        if (system_cursor_ + size + kPage > kSystemSize) return 0;
        va = kSystemBase + system_cursor_;
        system_cursor_ += size + kPage;   // one page of gap: a guard that stays reserved
    }
    if (VirtualAlloc2(GetCurrentProcess(), ptr(va), size, MEM_COMMIT, host_protect(protect), nullptr, 0) != ptr(va)) {
        system_free_.emplace(size, va);
        return 0;
    }
    system_[va] = {size, protect};
    *counter += size / kPage;
    return va;
}

void GuestMemory::release_system_block(uint32_t va, uint32_t size, uint32_t *counter)
{
    VirtualFree(ptr(va), size, MEM_DECOMMIT);
    system_.erase(va);
    system_free_.emplace(size, va);
    *counter -= size / kPage;
}

GuestMemory::Stack GuestMemory::allocate_stack(uint32_t size)
{
    size = round_up(size < kStackMin ? kStackMin : size, kPage);
    std::lock_guard<std::mutex> hold(lock_);
    uint32_t limit = system_block(size, xbox_page::ReadWrite, &stack_pages_);
    if (!limit) return {};
    return {{limit + size}, {limit}};
}

void GuestMemory::free_stack(const Stack &s)
{
    std::lock_guard<std::mutex> hold(lock_);
    auto it = system_.find(s.limit.v);
    if (it != system_.end()) release_system_block(s.limit.v, it->second.size, &stack_pages_);
}

GuestVa GuestMemory::allocate_system(uint32_t size, uint32_t protect)
{
    if (size == 0 || !valid_protect(protect)) return {};
    std::lock_guard<std::mutex> hold(lock_);
    return {system_block(round_up(size, kPage), protect, &system_pages_)};
}

void GuestMemory::free_system(GuestVa va, uint32_t)
{
    std::lock_guard<std::mutex> hold(lock_);
    auto it = system_.find(va.v);
    if (it != system_.end()) release_system_block(va.v, it->second.size, &system_pages_);
}

GuestVa GuestMemory::alloc_thread_block()
{
    std::lock_guard<std::mutex> hold(lock_);
    if (!thread_blocks_free_.empty()) {
        uint32_t va = thread_blocks_free_.back();
        thread_blocks_free_.pop_back();
        memset(ptr(va), 0, kThreadBlockSize);
        return {va};
    }
    return runtime_alloc(kThreadBlockSize, kPage, false);
}

void GuestMemory::free_thread_block(GuestVa block)
{
    std::lock_guard<std::mutex> hold(lock_);
    if (block.v) thread_blocks_free_.push_back(block.v);
}

uint32_t GuestMemory::physical(GuestVa va) const
{
    if (va.v >= kUserBase && va.v < ram_) return va.v;
    if (va.v >= kContiguousBase && va.v - kContiguousBase < ram_) return va.v - kContiguousBase;
    if (va.v >= kTiledBase && va.v - kTiledBase < ram_) return va.v - kTiledBase;
    return 0;
}

uint32_t GuestMemory::allocation_size(GuestVa va) const
{
    std::lock_guard<std::mutex> hold(lock_);
    if (auto it = contiguous_.find(va.v); it != contiguous_.end()) return it->second.size;
    if (auto it = system_.find(va.v); it != system_.end()) return it->second.size;
    bool overflow = false;
    auto vad = vad_containing(va.v & ~(kPage - 1), kPage, &overflow);
    return vad == vads_.end() ? 0 : vad->second.size;
}

GuestMemory::Statistics GuestMemory::statistics() const
{
    std::lock_guard<std::mutex> hold(lock_);
    Statistics s{};
    s.total_pages = ram_ / kPage;
    for (Owner o : pfn_) {
        if (o == Owner::User || o == Owner::Image) s.user_pages++;
        if (o == Owner::Contiguous) s.contiguous_pages++;
    }
    s.system_pages = system_pages_;
    s.stack_pages = stack_pages_;
    s.pool_pages = 0;
    s.available_pages = s.total_pages - s.user_pages - s.contiguous_pages - s.system_pages - s.stack_pages;
    return s;
}

void GuestMemory::set_no_access(uint32_t base, uint32_t size)
{
    // A reserved page already faults; only committed pages (RAM views, system memory) need the protection.
    MEMORY_BASIC_INFORMATION info{};
    for (uint32_t va = base & ~(kPage - 1); va < base + size; va += uint32_t(info.RegionSize ? info.RegionSize : kPage)) {
        if (!VirtualQuery(ptr(va), &info, sizeof info)) break;
        if (info.State == MEM_COMMIT) {
            uint32_t end = std::min<uint64_t>(uint64_t(va) + info.RegionSize, uint64_t(base) + size);
            protect_pages(va, end - va, PAGE_NOACCESS);
        }
    }
}

Parsed<void> GuestMemory::map_image(const XbeImage &xbe, std::span<const uint8_t> file)
{
    if (xbe.base.v != kUserBase) return std::unexpected(format("image base 0x%08X is not 0x%08X", xbe.base.v, kUserBase));
    if (xbe.image_size == 0 || xbe.image_size > ram_ - kUserBase)
        return std::unexpected(format("image size 0x%08X does not fit in %u MB", xbe.image_size, ram_ >> 20));
    uint32_t end = kUserBase + round_up(xbe.image_size, kReservationGranularity);
    std::lock_guard<std::mutex> hold(lock_);
    for (uint32_t p = kUserBase / kPage; p < end / kPage; p++)
        if (pfn_[p] != Owner::Free) return std::unexpected(format("image page 0x%08X is already owned", p * kPage));
    for (uint32_t p = kUserBase / kPage; p < end / kPage; p++) pfn_[p] = Owner::Reserved;
    vads_[kUserBase] = {end - kUserBase, xbox_page::ExecuteReadWrite};
    for (uint32_t va = kUserBase; va < end; va += kPage) commit_user(va, xbox_page::ExecuteReadWrite, true);
    memcpy(ptr(kUserBase), xbe.headers.data(), std::min<size_t>(xbe.headers.size(), end - kUserBase));
    for (const XbeSection &s : xbe.sections) {
        if (!s.preload) continue;
        if (s.va.v < kUserBase || s.va.v + s.virtual_size > end || s.raw_offset + s.raw_size > file.size())
            return std::unexpected(format("section %s 0x%08X+0x%X is outside the image", s.name.c_str(), s.va.v, s.virtual_size));
        uint32_t raw = std::min(s.raw_size, s.virtual_size);
        memcpy(ptr(s.va.v), file.data() + s.raw_offset, raw);
        if (s.virtual_size > raw) memset(ptr(s.va.v + raw), 0, s.virtual_size - raw);
    }
    FlushInstructionCache(GetCurrentProcess(), ptr(kUserBase), end - kUserBase);
    return {};
}

}  // namespace xbrt
