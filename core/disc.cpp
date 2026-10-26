#include "core/disc.h"

#include <algorithm>
#include <cstring>
#include <set>

namespace xbrt {

namespace {

constexpr uint64_t kSector = 2048;
constexpr char kMagic[] = "MICROSOFT*XBOX*MEDIA";
constexpr size_t kMagicLen = sizeof kMagic - 1;
// Partition bases the Mac lever (xbox-runtime/scripts/xiso_extract.py) found all four local images under: a bare
// XISO, the redump game partition after the video partition, and two rebuilt layouts. Route 2 also listed 0xFD90
// and 0x30600, which are not sector-aligned and matched nothing; they are gone.
constexpr uint64_t kBases[] = {0x00000000, 0x18300000, 0x0FD90000, 0x02080000};
constexpr uint32_t kEntryHeader = 0x0E;
constexpr uint8_t kAttrDirectory = 0x10;
constexpr uint32_t kMaxDirectoryBytes = 16u << 20;
constexpr size_t kMaxEntries = 1u << 20;
constexpr uint64_t kMaxReadAll = 128u << 20;

uint16_t u16(const uint8_t *p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t u32(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

bool plain_component(const std::string &name)
{
    if (name.empty() || name == "." || name == "..") return false;
    for (unsigned char c : name)
        if (c < 0x20 || c > 0x7E || c == '\\' || c == '/' || c == ':') return false;
    return true;
}

char lower(char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; }

bool path_less(std::string_view a, std::string_view b)
{
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
                                        [](char x, char y) { return lower(x) < lower(y); });
}

bool path_equal(std::string_view a, std::string_view b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return lower(x) == lower(y); });
}

std::string_view strip_root(std::string_view path) { return path.starts_with('\\') ? path.substr(1) : path; }

struct Dir {
    uint32_t sector;
    uint32_t size;
    std::string prefix;
};

}  // namespace

Parsed<DiscIndex> DiscIndex::open(const Source &image)
{
    const uint64_t image_size = image.size();
    uint64_t base = 0;
    uint8_t desc[kSector];
    bool found = false;
    for (uint64_t b : kBases) {
        if (b + 33 * kSector > image_size) continue;
        if (!image.read_at(b + 32 * kSector, desc)) return std::unexpected("read failed at the volume descriptor");
        if (std::memcmp(desc, kMagic, kMagicLen) == 0) {
            base = b;
            found = true;
            break;
        }
    }
    if (!found)
        return std::unexpected("not an Xbox disc image (no MICROSOFT*XBOX*MEDIA at sector 32 of any known partition base)");

    auto extent_ok = [&](uint32_t sector, uint64_t size) {
        uint64_t start = base + uint64_t(sector) * kSector;
        return start <= image_size && size <= image_size - start;
    };

    DiscIndex index;
    index.image_ = &image;
    std::vector<DiscFile> &files = index.files_;
    std::vector<Dir> pending{{u32(desc + 0x14), u32(desc + 0x18), ""}};
    std::set<uint32_t> seen;
    std::vector<uint8_t> data;
    while (!pending.empty()) {
        Dir dir = std::move(pending.back());
        pending.pop_back();
        if (dir.size == 0) continue;
        if (!seen.insert(dir.sector).second)
            return std::unexpected("directory at sector " + std::to_string(dir.sector) + " is reached twice");
        if (dir.size > kMaxDirectoryBytes || !extent_ok(dir.sector, dir.size))
            return std::unexpected("directory \"" + dir.prefix + "\" lies outside the image");
        data.resize(dir.size);
        if (!image.read_at(base + uint64_t(dir.sector) * kSector, data))
            return std::unexpected("read failed in directory \"" + dir.prefix + "\"");
        for (size_t at = 0; at < data.size(); at += kSector) {
            const uint8_t *chunk = data.data() + at;
            size_t len = std::min<size_t>(kSector, data.size() - at);
            for (size_t off = 0; off + kEntryHeader <= len;) {
                const uint8_t *e = chunk + off;
                uint8_t name_len = e[0x0D];
                if ((u16(e) == 0xFFFF && u16(e + 2) == 0xFFFF) || name_len == 0) break;
                if (off + kEntryHeader + name_len > len)
                    return std::unexpected("entry in \"" + dir.prefix + "\" runs past the end of its sector");
                std::string name(reinterpret_cast<const char *>(e + kEntryHeader), name_len);
                if (!plain_component(name))
                    return std::unexpected("entry in \"" + dir.prefix + "\" has a name that is not a plain file name");
                uint32_t sector = u32(e + 4), size = u32(e + 8);
                bool directory = e[0x0C] & kAttrDirectory;
                if (!directory && !extent_ok(sector, size))
                    return std::unexpected("file \"" + dir.prefix + name + "\" lies outside the image");
                if (files.size() == kMaxEntries) return std::unexpected("more entries than any Xbox disc holds");
                files.push_back({dir.prefix + name, base + uint64_t(sector) * kSector, size, directory});
                if (directory) pending.push_back({sector, size, dir.prefix + name + "\\"});
                off += (kEntryHeader + name_len + 3) & ~size_t(3);
            }
        }
    }
    std::sort(files.begin(), files.end(), [](const DiscFile &a, const DiscFile &b) { return path_less(a.path, b.path); });
    auto twin = std::adjacent_find(files.begin(), files.end(),
                                   [](const DiscFile &a, const DiscFile &b) { return path_equal(a.path, b.path); });
    if (twin != files.end()) return std::unexpected("two entries are named \"" + twin->path + "\"");
    return index;
}

std::optional<DiscFile> DiscIndex::find(std::string_view path) const
{
    path = strip_root(path);
    auto it = std::lower_bound(files_.begin(), files_.end(), path,
                               [](const DiscFile &f, std::string_view p) { return path_less(f.path, p); });
    if (it == files_.end() || !path_equal(it->path, path)) return std::nullopt;
    return *it;
}

std::vector<DiscFile> DiscIndex::list(std::string_view directory) const
{
    std::string prefix(strip_root(directory));
    if (!prefix.empty() && !prefix.ends_with('\\')) prefix += '\\';
    std::vector<DiscFile> out;
    auto it = std::lower_bound(files_.begin(), files_.end(), prefix,
                               [](const DiscFile &f, const std::string &p) { return path_less(f.path, p); });
    for (; it != files_.end() && path_equal(std::string_view(it->path).substr(0, prefix.size()), prefix); ++it)
        if (it->path.find('\\', prefix.size()) == std::string::npos) out.push_back(*it);
    return out;
}

Parsed<std::vector<uint8_t>> DiscIndex::read_all(const DiscFile &f) const
{
    if (f.directory) return std::unexpected("\"" + f.path + "\" is a directory");
    if (f.size > kMaxReadAll)
        return std::unexpected("\"" + f.path + "\" is " + std::to_string(f.size) + " bytes, more than any XBE");
    std::vector<uint8_t> out(size_t(f.size));
    if (!read(f, 0, out)) return std::unexpected("read failed in \"" + f.path + "\"");
    return out;
}

bool DiscIndex::read(const DiscFile &f, uint64_t at, std::span<uint8_t> out) const
{
    if (f.directory || at > f.size || out.size() > f.size - at) return false;
    return image_->read_at(f.offset + at, out);
}

}  // namespace xbrt
