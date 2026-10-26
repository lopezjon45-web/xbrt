#pragma once

#include "core/types.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xbrt {

struct DiscFile {
    std::string path;   // as the disc names it, backslash-separated, no leading slash ("media\\fe\\menu.big")
    uint64_t offset;    // of the data in the image (partition base + sector * 2048)
    uint64_t size;
    bool directory;
    uint8_t attributes;   // the XDVDFS attribute byte (0x10 directory, 0x01 read-only, ...)
};

class DiscIndex {
public:
    // Finds the game partition (the volume descriptor at sector 32 of one of the known partition bases), then reads
    // every directory. Names must be plain components, extents must lie inside the image, and a directory reached
    // twice ends the walk, so a corrupt image returns an error instead of looping. Keeps `image` by reference.
    static Parsed<DiscIndex> open(const Source &image);

    // Case-insensitive, as XDVDFS is. Accepts "\\default.xbe" and "default.xbe"; "D:\\default.xbe" is not accepted
    // (the kernel strips the device prefix before asking).
    std::optional<DiscFile> find(std::string_view path) const;
    // The same lookup as an index into all(); "" is not an entry (the root has none).
    std::optional<size_t> index_of(std::string_view path) const;
    std::vector<DiscFile> list(std::string_view directory) const;   // "" for the root
    const std::vector<DiscFile> &all() const { return files_; }

    // Whole small file (the XBE).
    Parsed<std::vector<uint8_t>> read_all(const DiscFile &f) const;
    bool read(const DiscFile &f, uint64_t at, std::span<uint8_t> out) const;

private:
    const Source *image_ = nullptr;
    std::vector<DiscFile> files_;
};

}  // namespace xbrt
