#include "core/xbe.h"

#include "core/sha256.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string_view>

namespace xbrt {

constexpr uint32_t kMaxImageSize = 128u << 20;   // no XBE exceeds the devkit RAM; the code map allocates by it

#include "core/gen/kernel_exports.inc"

namespace {

constexpr size_t kBaseField = 0x104, kHeadersSizeField = 0x108, kImageSizeField = 0x10C, kCertificateField = 0x118,
                 kSectionCountField = 0x11C, kSectionHeadersField = 0x120, kInitFlagsField = 0x124,
                 kEntryField = 0x128, kTlsField = 0x12C, kStackCommitField = 0x130, kKernelThunkField = 0x158,
                 kLibraryCountField = 0x160, kLibrariesField = 0x164, kHeaderEnd = 0x178;
constexpr uint32_t kBase = 0x10000;
constexpr size_t kSectionHeaderSize = 0x38, kLibrarySize = 16, kTlsSize = 24, kCertificateSize = 0xA4;
constexpr size_t kTitleNameUnits = 40;

struct KeyPair {
    uint32_t entry, thunk;
};
constexpr KeyPair kRetailKeys{0xA8FC57AB, 0x5B6D40B6}, kDebugKeys{0x94859D4B, 0xEFB1F152},
    kChihiroKeys{0x40B5C16E, 0x2290059D};

uint32_t u32(std::span<const uint8_t> f, size_t off)
{
    return uint32_t(f[off]) | uint32_t(f[off + 1]) << 8 | uint32_t(f[off + 2]) << 16 | uint32_t(f[off + 3]) << 24;
}

uint16_t u16(std::span<const uint8_t> f, size_t off) { return uint16_t(f[off] | f[off + 1] << 8); }

std::unexpected<std::string> fail(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    return std::unexpected(std::string(line));
}

XbeKeys key_kind(uint32_t entry_raw)
{
    if ((entry_raw & 0xF0000000) == 0x40000000) return XbeKeys::Chihiro;
    if ((entry_raw ^ kDebugKeys.entry) < 0x4000000) return XbeKeys::Debug;
    return XbeKeys::Retail;
}

KeyPair keys_of(XbeKeys k)
{
    switch (k) {
    case XbeKeys::Retail: return kRetailKeys;
    case XbeKeys::Debug: return kDebugKeys;
    case XbeKeys::Chihiro: return kChihiroKeys;
    }
    return kRetailKeys;
}

std::optional<std::string> utf8_from_utf16(std::span<const uint8_t> bytes, size_t units)
{
    std::string out;
    for (size_t i = 0; i < units; i++) {
        uint32_t c = u16(bytes, i * 2);
        if (c == 0) break;
        if (c >= 0xDC00 && c <= 0xDFFF) return std::nullopt;
        if (c >= 0xD800 && c <= 0xDBFF) {
            if (++i == units) return std::nullopt;
            uint32_t lo = u16(bytes, i * 2);
            if (lo < 0xDC00 || lo > 0xDFFF) return std::nullopt;
            c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
        }
        if (c < 0x80) {
            out += char(c);
        } else if (c < 0x800) {
            out += char(0xC0 | c >> 6);
            out += char(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += char(0xE0 | c >> 12);
            out += char(0x80 | (c >> 6 & 0x3F));
            out += char(0x80 | (c & 0x3F));
        } else {
            out += char(0xF0 | c >> 18);
            out += char(0x80 | (c >> 12 & 0x3F));
            out += char(0x80 | (c >> 6 & 0x3F));
            out += char(0x80 | (c & 0x3F));
        }
    }
    return out;
}

}  // namespace

std::string_view kernel_export_name(Ordinal o)
{
    return o.v < std::size(kKernelExports) ? kKernelExports[o.v] : std::string_view{};
}

const XbeSection *XbeImage::section_at(GuestVa va) const
{
    for (const XbeSection &s : sections)
        if (va.v - s.va.v < s.virtual_size) return &s;
    return nullptr;
}

uint32_t XbeImage::tls_block_size() const
{
    if (!tls.index_var.v) return 0;
    return ((tls.raw_end.v - tls.raw_start.v + tls.zero_fill + 15) & ~15u) + 4;
}

std::string XbeImage::title_id_hex() const
{
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08X", cert.title_id);
    return buf;
}

Parsed<XbeImage> parse_xbe(std::span<const uint8_t> file)
{
    if (file.size() < kHeaderEnd)
        return fail("file size: 0x%zX bytes is shorter than the XBE image header (0x%zX)", file.size(), kHeaderEnd);
    if (std::memcmp(file.data(), "XBEH", 4) != 0) return fail("magic: the file does not start with XBEH");

    XbeImage x{};
    x.base = {u32(file, kBaseField)};
    if (x.base.v != kBase) return fail("base: 0x%X, every XBE loads at 0x%X", x.base.v, kBase);
    uint32_t headers_size = u32(file, kHeadersSizeField);
    if (headers_size < kHeaderEnd || headers_size > file.size())
        return fail("headers_size: 0x%X is outside [0x%zX, file size 0x%zX]", headers_size, kHeaderEnd, file.size());
    x.image_size = u32(file, kImageSizeField);
    uint64_t image_end = uint64_t(x.base.v) + x.image_size;
    if (x.image_size < headers_size || image_end > 0x100000000)
        return fail("image_size: 0x%X does not hold the 0x%X header bytes below 4 GB", x.image_size, headers_size);
    if (x.image_size > kMaxImageSize) return fail("image_size: 0x%X is larger than the devkit's RAM", x.image_size);
    x.headers.assign(file.begin(), file.begin() + headers_size);

    auto header_off = [&](uint32_t va, uint64_t len) -> std::optional<size_t> {
        if (va < x.base.v || va - x.base.v > headers_size || len > headers_size - (va - x.base.v)) return std::nullopt;
        return size_t(va - x.base.v);
    };
    auto file_off = [&](uint32_t va, uint64_t len) -> std::optional<size_t> {
        if (std::optional<size_t> h = header_off(va, len)) return h;
        for (const XbeSection &s : x.sections) {
            uint32_t d = va - s.va.v;
            if (va >= s.va.v && d < s.raw_size && len <= s.raw_size - d) return size_t(s.raw_offset) + d;
        }
        return std::nullopt;
    };

    uint32_t cert_addr = u32(file, kCertificateField);
    std::optional<size_t> cert = header_off(cert_addr, kCertificateSize);
    if (!cert) return fail("certificate_addr: 0x%X is not inside the headers", cert_addr);
    x.cert.title_id = u32(file, *cert + 8);
    std::optional<std::string> title = utf8_from_utf16(file.subspan(*cert + 0xC), kTitleNameUnits);
    if (!title) return fail("certificate title_name: not valid UTF-16");
    x.cert.title_name = std::move(*title);
    x.cert.game_region = u32(file, *cert + 0xA0);

    uint32_t section_count = u32(file, kSectionCountField);
    uint32_t section_headers = u32(file, kSectionHeadersField);
    std::optional<size_t> table = header_off(section_headers, uint64_t(section_count) * kSectionHeaderSize);
    if (!table)
        return fail("section_headers_addr: %u section headers at 0x%X do not fit in the headers", section_count,
                    section_headers);
    for (uint32_t i = 0; i < section_count; i++) {
        size_t h = *table + i * kSectionHeaderSize;
        uint32_t flags = u32(file, h);
        XbeSection s{};
        s.va = {u32(file, h + 4)};
        s.virtual_size = u32(file, h + 8);
        s.raw_offset = u32(file, h + 12);
        s.raw_size = u32(file, h + 16);
        uint32_t name_addr = u32(file, h + 20);
        std::optional<size_t> name = header_off(name_addr, 0);
        const void *nul = name ? std::memchr(file.data() + *name, 0, headers_size - *name) : nullptr;
        if (!nul) return fail("section %u name_addr: 0x%X is not a terminated string in the headers", i, name_addr);
        s.name.assign(reinterpret_cast<const char *>(file.data() + *name), static_cast<const char *>(nul));
        if (uint64_t(s.raw_offset) + s.raw_size > file.size())
            return fail("section %u (%s) raw extent: 0x%X+0x%X is past the end of the file (0x%zX)", i,
                        s.name.c_str(), s.raw_offset, s.raw_size, file.size());
        if (s.va.v < x.base.v || uint64_t(s.va.v) + s.virtual_size > image_end)
            return fail("section %u (%s) virtual extent: 0x%X+0x%X is outside the image [0x%X, 0x%llX)", i,
                        s.name.c_str(), s.va.v, s.virtual_size, x.base.v, (unsigned long long)image_end);
        s.writable = flags & 0x1;
        s.preload = flags & 0x2;
        s.executable = flags & 0x4;
        s.inserted_file = flags & 0x8;
        s.head_page_ro = flags & 0x10;
        s.tail_page_ro = flags & 0x20;
        x.sections.push_back(std::move(s));
    }

    uint32_t entry_raw = u32(file, kEntryField);
    x.keys = key_kind(entry_raw);
    KeyPair keys = keys_of(x.keys);
    x.entry = {entry_raw ^ keys.entry};
    if (!x.section_at(x.entry)) return fail("entry: 0x%X is not inside a section", x.entry.v);

    x.thunk_table = {u32(file, kKernelThunkField) ^ keys.thunk};
    const XbeSection *thunk_section = x.section_at(x.thunk_table);
    if (!thunk_section) return fail("kernel_thunk: 0x%X is not inside a section", x.thunk_table.v);
    uint32_t thunk_end = std::min(thunk_section->raw_size, thunk_section->virtual_size);
    for (uint32_t d = x.thunk_table.v - thunk_section->va.v;; d += 4) {
        uint32_t slot = thunk_section->va.v + d;
        if (uint64_t(d) + 4 > thunk_end)
            return fail("kernel_thunk imports: no zero terminator before 0x%X, the end of %s's file data", slot,
                        thunk_section->name.c_str());
        uint32_t v = u32(file, size_t(thunk_section->raw_offset) + d);
        if (v == 0) break;
        uint32_t ordinal = v & 0x7FFFFFFF;
        if (!(v & 0x80000000) || ordinal == 0 || ordinal > 0xFFFF)
            return fail("kernel_thunk imports: slot 0x%X holds 0x%08X, not an ordinal import", slot, v);
        x.imports.push_back({uint16_t(ordinal)});
    }

    uint32_t tls_addr = u32(file, kTlsField);
    if (tls_addr) {
        std::optional<size_t> tls = file_off(tls_addr, kTlsSize);
        if (!tls) return fail("tls_addr: 0x%X is not in the headers or a section's file data", tls_addr);
        x.tls.raw_start = {u32(file, *tls)};
        x.tls.raw_end = {u32(file, *tls + 4)};
        x.tls.index_var = {u32(file, *tls + 8)};
        x.tls.zero_fill = u32(file, *tls + 16);
        if (x.tls.raw_start != x.tls.raw_end &&
            (x.tls.raw_start > x.tls.raw_end || x.tls.raw_start.v < x.base.v || x.tls.raw_end.v > image_end))
            return fail("tls raw data: 0x%X..0x%X is not a range inside the image", x.tls.raw_start.v,
                        x.tls.raw_end.v);
        if (!x.section_at(x.tls.index_var) || !x.section_at(x.tls.index_var + 3))
            return fail("tls index_var: 0x%X is not inside a section", x.tls.index_var.v);
    }

    uint32_t library_count = u32(file, kLibraryCountField);
    uint32_t libraries = u32(file, kLibrariesField);
    std::optional<size_t> libs = header_off(libraries, uint64_t(library_count) * kLibrarySize);
    if (library_count && !libs)
        return fail("lib_versions_addr: %u library entries at 0x%X do not fit in the headers", library_count,
                    libraries);
    for (uint32_t i = 0; i < library_count; i++) {
        size_t l = *libs + i * kLibrarySize;
        const char *name = reinterpret_cast<const char *>(file.data() + l);
        const void *nul = std::memchr(name, 0, 8);
        uint16_t flags = u16(file, l + 14);
        x.libraries.push_back({std::string(name, nul ? static_cast<const char *>(nul) : name + 8), u16(file, l + 8), u16(file, l + 10),
                               u16(file, l + 12), uint16_t(flags & 0x1FFF), bool(flags >> 15)});
    }

    x.init_flags = u32(file, kInitFlagsField);
    x.stack_commit = u32(file, kStackCommitField);
    x.sha256 = sha256(file);
    return x;
}

}  // namespace xbrt
