#pragma once

#include "core/types.h"
#include "core/xbe.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xbrt {

struct Symbol {
    std::string library;
    std::string name;
    GuestVa va;
    uint16_t build;        // the XDK build the signature matched
    bool is_function;
    bool is_offset;
};

class SymbolMap {
public:
    explicit SymbolMap(std::vector<Symbol> symbols);
    SymbolMap() = default;

    std::optional<Symbol> find(std::string_view library, std::string_view name) const;
    // The nearest function at or below `va`, for naming a guest address ("D3D8 CMiniport_InitHardware+0x12C").
    std::optional<Symbol> nearest_function(GuestVa va) const;
    // "XAPILIB QueryPerformanceCounter+0x2A", or "<section>+0x<offset>" when no function is near enough.
    std::string describe(GuestVa va, const XbeImage &xbe) const;
    std::span<const Symbol> all() const { return symbols_; }
    uint32_t functions_in(std::string_view library) const;

private:
    std::vector<Symbol> symbols_;
};

// Runs the database over the raw file (sections at their file offsets). Deterministic; the same on every host.
SymbolMap scan_symbols(const XbeImage &xbe, std::span<const uint8_t> file);

// Whether the database carries signatures for this library name at all (D3DX8: no; XNET: yes).
bool database_knows_library(std::string_view library);

}  // namespace xbrt
