#include "core/symbols.h"

#include <libXbSymbolDatabase.h>

#include <algorithm>
#include <cstdio>
#include <tuple>

namespace xbrt {

namespace {

constexpr uint32_t kNearEnough = 0x4000;

// The database's register callback carries no user pointer.
thread_local std::vector<Symbol> *g_sink;

void collect(const char *library_str, uint32_t, uint32_t, const char *symbol_str, xbaddr address,
             uint32_t build_version, uint32_t symbol_type, uint32_t, uint32_t, const XbSDBSymbolParam *)
{
    Symbol s;
    s.library.assign(library_str, std::find(library_str, library_str + 8, '\0'));
    s.name = symbol_str;
    s.va = {address};
    s.build = uint16_t(build_version);
    s.is_function = symbol_type == symbol_function;
    s.is_offset = s.name.ends_with("_OFFSET");
    g_sink->push_back(std::move(s));
}

void quiet(xb_output_message, const char *) {}

auto key(const Symbol &s) { return std::tie(s.va.v, s.library, s.name); }

}  // namespace

SymbolMap::SymbolMap(std::vector<Symbol> symbols) : symbols_(std::move(symbols))
{
    std::sort(symbols_.begin(), symbols_.end(), [](const Symbol &a, const Symbol &b) { return key(a) < key(b); });
    symbols_.erase(std::unique(symbols_.begin(), symbols_.end(),
                               [](const Symbol &a, const Symbol &b) { return key(a) == key(b); }),
                   symbols_.end());
}

std::optional<Symbol> SymbolMap::find(std::string_view library, std::string_view name) const
{
    for (const Symbol &s : symbols_)
        if (s.library == library && s.name == name) return s;
    return std::nullopt;
}

std::optional<Symbol> SymbolMap::nearest_function(GuestVa va) const
{
    auto it = std::upper_bound(symbols_.begin(), symbols_.end(), va.v,
                               [](uint32_t v, const Symbol &s) { return v < s.va.v; });
    while (it != symbols_.begin()) {
        --it;
        if (it->is_function && !it->is_offset) return *it;
    }
    return std::nullopt;
}

std::string SymbolMap::describe(GuestVa va, const XbeImage &xbe) const
{
    const XbeSection *section = xbe.section_at(va);
    std::optional<Symbol> f = nearest_function(va);
    char buf[128];
    if (f && va.v - f->va.v < kNearEnough && section && section == xbe.section_at(f->va)) {
        std::snprintf(buf, sizeof buf, "%s %s+0x%X", f->library.c_str(), f->name.c_str(), va.v - f->va.v);
        return buf;
    }
    if (section) {
        std::snprintf(buf, sizeof buf, "%s+0x%X", section->name.c_str(), va.v - section->va.v);
        return buf;
    }
    std::snprintf(buf, sizeof buf, "0x%08X", va.v);
    return buf;
}

uint32_t SymbolMap::functions_in(std::string_view library) const
{
    return uint32_t(std::count_if(symbols_.begin(), symbols_.end(), [&](const Symbol &s) {
        return s.is_function && !s.is_offset && s.library == library;
    }));
}

SymbolMap scan_symbols(const XbeImage &xbe, std::span<const uint8_t> file)
{
    std::vector<Symbol> found;
    g_sink = &found;
    XbSDB_SetOutputMessage(quiet);
    // False means the XBE links no library the database knows: no symbols, not an error.
    XbSDB_Scan(file.data(), collect, true);
    g_sink = nullptr;
    // Not every offset carries the _OFFSET suffix (XAPILIB _tls_array is 4, an offset into the fs: block), but no
    // variable address lies outside the image.
    for (Symbol &s : found)
        if (!s.is_function && s.va.v - xbe.base.v >= xbe.image_size) s.is_offset = true;
    return SymbolMap(std::move(found));
}

bool database_knows_library(std::string_view library)
{
    // XbSDB_LibraryToFlag also answers for libraries it has a flag but no signatures for (D3DX8), so compare the
    // signature count with the library's against the count for no library.
    uint32_t flag = XbSDB_LibraryToFlag(std::string(library).c_str());
    return flag != 0 && XbSDB_GetTotalSymbols(flag) > XbSDB_GetTotalSymbols(0);
}

}  // namespace xbrt
