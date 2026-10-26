#pragma once

#include <compare>
#include <cstdint>
#include <expected>
#include <span>
#include <string>

namespace xbrt {

// A guest virtual address (always below 4 GB). Never a host pointer: host code turns one into a pointer only through
// GuestMemory::host<T>(), which knows the guest map sits at host address == guest address.
struct GuestVa {
    uint32_t v = 0;
    auto operator<=>(const GuestVa &) const = default;
    GuestVa operator+(uint32_t d) const { return {v + d}; }
};

// xboxkrnl export ordinal (1..379 on retail kernels).
struct Ordinal {
    uint16_t v;
    auto operator<=>(const Ordinal &) const = default;
};

// A file-like source read at absolute offsets. read_at returns false on a short read or an I/O error.
struct Source {
    virtual bool read_at(uint64_t offset, std::span<uint8_t> out) const = 0;
    virtual uint64_t size() const = 0;
    virtual ~Source() = default;
};

// The string says what was wrong, in one line, for run.txt and the status screen.
template <class T>
using Parsed = std::expected<T, std::string>;

}  // namespace xbrt
