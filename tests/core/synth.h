#pragma once

#include "core/types.h"

#include <cstring>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace synth {

inline void put16(std::vector<uint8_t> &b, size_t off, uint16_t v) { std::memcpy(b.data() + off, &v, 2); }
inline void put32(std::vector<uint8_t> &b, size_t off, uint32_t v) { std::memcpy(b.data() + off, &v, 4); }
inline uint32_t get32(const std::vector<uint8_t> &b, size_t off)
{
    uint32_t v;
    std::memcpy(&v, b.data() + off, 4);
    return v;
}

constexpr uint32_t kBase = 0x10000, kTextVa = 0x11000, kRdataVa = 0x12000, kRdataVsize = 0x100;
constexpr uint32_t kTlsIndexVar = kRdataVa + 0x40, kTlsZeroFill = 0x90;
constexpr size_t kCertOff = 0x184, kSectOff = 0x354, kStringsOff = 0x400, kLibsOff = 0x500, kTlsOff = 0x600;
constexpr size_t kBaseField = 0x104, kHeadersSizeField = 0x108, kCertificateField = 0x118, kSectionCountField = 0x11C,
                 kEntryField = 0x128, kKernelThunkField = 0x158, kLibraryCountField = 0x160;
constexpr uint32_t kOrdinals[] = {1, 187, 294};
constexpr uint8_t kText[] = {0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x0F, 0x09, 0xC3};
constexpr size_t kThunkFileOff = 0x2000;

struct Keys {
    uint32_t entry, thunk;
};
constexpr Keys kRetail{0xA8FC57AB, 0x5B6D40B6}, kDebug{0x94859D4B, 0xEFB1F152}, kChihiro{0x40B5C16E, 0x2290059D};

inline size_t section_header(int i) { return kSectOff + size_t(i) * 0x38; }

inline std::vector<uint8_t> build_xbe(Keys keys = kRetail, std::u16string_view title = u"SYNTH",
                                      uint32_t title_id = 0xFFFF0001)
{
    std::vector<uint8_t> x(0x3000, 0);
    const char *names[] = {".text", ".rdata"};
    for (int i = 0; i < 2; i++) std::memcpy(x.data() + kStringsOff + i * 16, names[i], std::strlen(names[i]) + 1);
    std::memcpy(x.data(), "XBEH", 4);
    const uint32_t fields[29] = {kBase, 0x1000, 0x3000, 0x184, 0x4123E164, kBase + uint32_t(kCertOff), 2,
                                 kBase + uint32_t(kSectOff), 0x5, kTextVa ^ keys.entry, kBase + uint32_t(kTlsOff),
                                 0x10000, 0x100000, 0x1000, kBase, 0x3000, 0, 0x4123E164, 0, 0, 0,
                                 kRdataVa ^ keys.thunk, 0, 2, kBase + uint32_t(kLibsOff), 0, 0, 0, 0};
    for (int i = 0; i < 29; i++) put32(x, 0x104 + i * 4, fields[i]);

    put32(x, kCertOff, 0x1D0);
    put32(x, kCertOff + 4, 0x4123E164);
    put32(x, kCertOff + 8, title_id);
    for (size_t i = 0; i < title.size() && i < 40; i++) put16(x, kCertOff + 0xC + i * 2, uint16_t(title[i]));
    put32(x, kCertOff + 0xA0, 1);

    const uint32_t thunk_bytes = 4 * (std::size(kOrdinals) + 1);
    const uint32_t sections[2][5] = {{0x6, kTextVa, sizeof kText, 0x1000, sizeof kText},
                                     {0x2, kRdataVa, kRdataVsize, uint32_t(kThunkFileOff), thunk_bytes}};
    for (int i = 0; i < 2; i++) {
        for (int f = 0; f < 5; f++) put32(x, section_header(i) + f * 4, sections[i][f]);
        put32(x, section_header(i) + 20, kBase + uint32_t(kStringsOff) + i * 16);
    }

    std::memcpy(x.data() + kLibsOff, "XAPILIB\0", 8);
    put16(x, kLibsOff + 8, 1);
    put16(x, kLibsOff + 12, 5849);
    put16(x, kLibsOff + 14, 1 | 2 << 13);
    std::memcpy(x.data() + kLibsOff + 16, "D3D8\0\0\0\0", 8);
    put16(x, kLibsOff + 16 + 8, 1);
    put16(x, kLibsOff + 16 + 10, 2);
    put16(x, kLibsOff + 16 + 12, 4627);
    put16(x, kLibsOff + 16 + 14, 3 | 0x8000);

    put32(x, kTlsOff + 8, kTlsIndexVar);
    put32(x, kTlsOff + 16, kTlsZeroFill);

    std::memcpy(x.data() + 0x1000, kText, sizeof kText);
    for (size_t i = 0; i < std::size(kOrdinals); i++) put32(x, kThunkFileOff + i * 4, kOrdinals[i] | 0x80000000);
    return x;
}

inline std::vector<uint8_t> with_text(std::vector<uint8_t> x, std::span<const uint8_t> code)
{
    std::memset(x.data() + 0x1000, 0, 0x1000);
    std::memcpy(x.data() + 0x1000, code.data(), code.size());
    put32(x, section_header(0) + 8, uint32_t(code.size()));
    put32(x, section_header(0) + 16, uint32_t(code.size()));
    return x;
}

constexpr uint64_t kSector = 2048;
constexpr uint8_t kDirAttr = 0x10;

struct Entry {
    std::string name;
    uint32_t sector, size;
    uint8_t attr = 0;
    uint16_t left = 0x1234, right = 0x0040;
};

struct Xiso final : xbrt::Source {
    uint64_t base = 0;
    std::vector<uint8_t> bytes;
    uint64_t reported_size = 0;

    explicit Xiso(uint64_t b) : base(b) {}

    uint64_t size() const override { return reported_size ? reported_size : base + bytes.size(); }
    bool read_at(uint64_t offset, std::span<uint8_t> out) const override
    {
        for (size_t i = 0; i < out.size(); i++) {
            uint64_t o = offset + i;
            if (o < base) {
                out[i] = 0;
                continue;
            }
            if (o - base >= bytes.size()) return false;
            out[i] = bytes[size_t(o - base)];
        }
        return true;
    }
    uint8_t *at(uint32_t sector, size_t len)
    {
        size_t off = size_t(sector) * kSector;
        if (bytes.size() < off + len) bytes.resize(off + len, 0);
        return bytes.data() + off;
    }
    void descriptor(uint32_t root_sector, uint32_t root_size)
    {
        uint8_t *d = at(32, kSector);
        std::memcpy(d, "MICROSOFT*XBOX*MEDIA", 20);
        std::memcpy(d + 0x14, &root_sector, 4);
        std::memcpy(d + 0x18, &root_size, 4);
        std::memcpy(d + 0x7EC, "MICROSOFT*XBOX*MEDIA", 20);
    }
    void directory(uint32_t first_sector, const std::vector<std::vector<Entry>> &sectors)
    {
        for (size_t s = 0; s < sectors.size(); s++) {
            uint8_t *p = at(first_sector + uint32_t(s), kSector);
            std::memset(p, 0xFF, kSector);
            size_t off = 0;
            for (const Entry &e : sectors[s]) {
                std::memcpy(p + off, &e.left, 2);
                std::memcpy(p + off + 2, &e.right, 2);
                std::memcpy(p + off + 4, &e.sector, 4);
                std::memcpy(p + off + 8, &e.size, 4);
                p[off + 12] = e.attr;
                p[off + 13] = uint8_t(e.name.size());
                std::memcpy(p + off + 14, e.name.data(), e.name.size());
                off += (14 + e.name.size() + 3) & ~size_t(3);
            }
        }
    }
    void file(uint32_t sector, const std::vector<uint8_t> &data)
    {
        std::memcpy(at(sector, data.size()), data.data(), data.size());
    }
};

inline Xiso game_like(uint64_t base)
{
    Xiso img(base);
    img.descriptor(33, 2048);
    img.directory(33, {{{"default.xbe", 40, 100}, {"a.txt", 41, 5}, {"media", 34, 4096, kDirAttr}, {"empty.dat", 0, 0}}});
    img.directory(34, {{{"x.big", 42, 3000}, {"Deep", 36, 2048, kDirAttr}}, {{"y.big", 44, 10}}});
    img.directory(36, {{{"z.bin", 45, 1}}});
    img.at(45, kSector);
    return img;
}

}  // namespace synth
