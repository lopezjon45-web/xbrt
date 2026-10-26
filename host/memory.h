// Map (every range reserved at its fixed address by reserve(), before anything else in the process allocates low):
//   0x00010000 .. RAM_END         user RAM: view of the RAM section at offset == VA (identity: physical == VA)
//   RAM_END .. 0x7FFDFFFF         left free for the host (drivers)
//   0x80000000 .. +RAM            contiguous: view of the same section at offset 0 (the rest to 0x90000000 reserved)
//   0x90000000 .. 0x90FFFFFF      runtime region: gates, id stubs, kernel data, ThreadBlocks (committed on demand)
//   0xC0000000 .. 0xC03FFFFF      page tables: reserved, no access
//   0xD0000000 .. 0xEFFFFFFF      system: guest thread stacks and pool (private memory)
//   0xF0000000 .. +RAM            tiled: third view of the RAM section (the rest to 0xFFC00000 reserved: MMIO)
//   0xFFC00000 .. 0xFFFEFFFF      flash: reserved
#pragma once

#include "core/types.h"
#include "core/xbe.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace xbrt {

enum class RamSize : uint32_t { Retail64 = 64u << 20, Devkit128 = 128u << 20 };

class GuestMemory {
public:
    // First thing in wWinMain. Reserves every range above as placeholders, creates the RAM section and maps its
    // three views. On failure the error names the range and the Win32 error. log() holds one line per step either
    // way, for run.txt.
    static Parsed<std::unique_ptr<GuestMemory>> reserve(RamSize ram);
    ~GuestMemory();

    const std::vector<std::string> &log() const { return log_; }
    uint32_t ram_bytes() const { return ram_; }

    // Writes a pattern at user VA 0x00100000 and reads it back at 0x80100000 and 0xF0100000, then checks that a
    // user page takes PAGE_EXECUTE_READWRITE through VirtualProtectFromApp. One line, ok or the failing step.
    std::string identity_check();

    // A guest thread's stack in system space: [limit, base) committed. The trap frame the kernel builds on a guest
    // stack and the VEH's own frames need headroom, so size is at least 64 KB.
    struct Stack {
        GuestVa base, limit;
    };
    Stack allocate_stack(uint32_t size);

    // Bump allocator inside the runtime region; pages are RWX when `exec`. Never freed except ThreadBlocks.
    GuestVa runtime_alloc(uint32_t size, uint32_t alignment, bool exec);
    GuestVa alloc_thread_block();   // kThreadBlockSize, from a free list
    void free_thread_block(GuestVa block);
    void free_stack(const Stack &s);

    // Headers at the base, each preload section at its VA (raw bytes then zero fill), all pages RWX (every Xbox
    // section is executable and titles patch their own code). Claims those physical pages in the PFN bitmap.
    Parsed<void> map_image(const XbeImage &xbe, std::span<const uint8_t> file);

    // The Xbox kernel's memory verbs, as the bridges' layer A calls them through host/kernel.cpp. Xbox NTSTATUS
    // values out (STATUS_SUCCESS 0, STATUS_NO_MEMORY 0xC0000017, STATUS_INVALID_PARAMETER 0xC000000D, ...).
    uint32_t allocate_virtual(GuestVa *base, uint32_t *size, uint32_t type, uint32_t protect);
    uint32_t free_virtual(GuestVa *base, uint32_t *size, uint32_t type);
    uint32_t protect_virtual(GuestVa *base, uint32_t *size, uint32_t protect, uint32_t *old_protect);
    uint32_t query_virtual(GuestVa va, struct XboxMemoryBasicInformation *out) const;
    GuestVa allocate_contiguous(uint32_t size, uint32_t lowest, uint32_t highest, uint32_t alignment,
                                uint32_t protect);   // 0x80000000 + physical, or 0
    void free_contiguous(GuestVa va);
    GuestVa allocate_system(uint32_t size, uint32_t protect);   // pool and MmAllocateSystemMemory
    void free_system(GuestVa va, uint32_t size);
    uint32_t physical(GuestVa va) const;      // exact for user RAM, contiguous and tiled; 0 otherwise
    uint32_t allocation_size(GuestVa va) const;
    struct Statistics {
        uint32_t total_pages, available_pages, contiguous_pages, user_pages, system_pages, stack_pages, pool_pages;
    };
    Statistics statistics() const;

    // Page protection for trapping device pages (Cpu::map_mmio calls this; Cpu owns the routing, memory the state).
    void set_no_access(uint32_t base, uint32_t size);

    template <class T>
    T *host(GuestVa va) const { return reinterpret_cast<T *>(static_cast<uintptr_t>(va.v)); }

    static constexpr uint32_t kThreadBlockSize = 0x2000;

private:
    GuestMemory() = default;
    std::vector<std::string> log_;
    uint32_t ram_ = 0;
    void *section_ = nullptr;
    uint32_t runtime_cursor_ = 0;
    uint32_t stack_cursor_ = 0;
};

}  // namespace xbrt
