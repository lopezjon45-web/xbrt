#include "core/disc_volume.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <optional>

namespace xbrt {

namespace {

constexpr uint8_t kAttrDirectory = 0x10;

}  // namespace

std::string disc_cache_line(const DiscCacheStats &s)
{
    char line[160];
    snprintf(line, sizeof line, "disc cache %llu reads: %llu block hits, %llu misses, %llu direct (%.1f MB), %llu ms in the image",
             (unsigned long long)s.reads, (unsigned long long)s.block_hits, (unsigned long long)s.block_misses,
             (unsigned long long)s.direct_reads, double(s.direct_bytes) / (1024.0 * 1024.0),
             (unsigned long long)(s.image_us / 1000));
    return line;
}

bool BlockCache::timed_read(uint64_t offset, std::span<uint8_t> out)
{
    auto start = std::chrono::steady_clock::now();
    bool ok = image_.read_at(offset, out);
    stats_.image_us += uint64_t(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
    return ok;
}

const BlockCache::Block *BlockCache::block(uint64_t index)
{
    if (auto it = where_.find(index); it != where_.end()) {
        stats_.block_hits++;
        lru_.splice(lru_.begin(), lru_, it->second);
        return &*it->second;
    }
    stats_.block_misses++;
    uint64_t start = index * kBlockBytes;
    std::vector<uint8_t> bytes(size_t(std::min(kBlockBytes, image_.size() - start)));
    if (!timed_read(start, bytes)) return nullptr;
    if (lru_.size() == kMaxBlocks) {
        where_.erase(lru_.back().index);
        lru_.pop_back();
    }
    lru_.push_front({index, std::move(bytes)});
    where_[index] = lru_.begin();
    return &lru_.front();
}

bool BlockCache::read(uint64_t offset, std::span<uint8_t> out)
{
    std::lock_guard g(lock_);
    stats_.reads++;
    uint64_t end = offset + out.size();
    if (end < offset || end > image_.size()) return false;
    uint64_t at = offset;
    while (at < end) {
        uint64_t index = at / kBlockBytes, block_start = index * kBlockBytes;
        uint64_t whole_end = end / kBlockBytes * kBlockBytes;
        if (at == block_start && whole_end > at) {
            stats_.direct_reads++;
            stats_.direct_bytes += whole_end - at;
            if (!timed_read(at, out.subspan(size_t(at - offset), size_t(whole_end - at)))) return false;
            at = whole_end;
            continue;
        }
        const Block *b = block(index);
        if (!b) return false;
        uint64_t stop = std::min(end, block_start + b->bytes.size());
        if (stop <= at) return false;
        std::memcpy(out.data() + (at - offset), b->bytes.data() + (at - block_start), size_t(stop - at));
        at = stop;
    }
    return true;
}

DiscCacheStats BlockCache::stats() const
{
    std::lock_guard g(lock_);
    return stats_;
}

DiscVolume::DiscVolume(const DiscIndex &index, const Source &image)
    : index_(index), cache_(image), root_(uint32_t(index.all().size())), children_(index.all().size() + 1)
{
    const std::vector<DiscFile> &all = index.all();
    for (uint32_t i = 0; i < all.size(); i++) {
        size_t slash = all[i].path.find_last_of('\\');
        std::optional<size_t> parent =
            slash == std::string::npos ? std::nullopt : index.index_of(std::string_view(all[i].path).substr(0, slash));
        children_[parent ? *parent : root_].push_back(i);
    }
    c_.find = &DiscVolume::find;
    c_.read = &DiscVolume::read;
    c_.child = &DiscVolume::child;
    c_.image_bytes = image.size();
    c_.self = this;
}

XboxDiscEntry DiscVolume::entry(uint32_t id) const
{
    if (id == root_) return {id, kAttrDirectory, 0};
    const DiscFile &f = index_.all()[id];
    uint8_t attributes = f.attributes | (f.directory ? kAttrDirectory : 0);
    return {id, attributes, f.directory ? 0 : f.size};
}

int DiscVolume::find(const XboxDiscVolume *v, const char *path, XboxDiscEntry *out)
{
    const DiscVolume &self = *static_cast<const CVolume *>(v)->self;
    if (!*path) {
        *out = self.entry(self.root_);
        return 1;
    }
    std::optional<size_t> i = self.index_.index_of(path);
    if (!i) return 0;
    *out = self.entry(uint32_t(*i));
    return 1;
}

int64_t DiscVolume::read(const XboxDiscVolume *v, uint32_t id, uint64_t offset, void *out, uint32_t len)
{
    DiscVolume &self = *static_cast<const CVolume *>(v)->self;
    if (id >= self.root_) return -1;
    const DiscFile &f = self.index_.all()[id];
    if (f.directory) return -1;
    if (offset >= f.size) return 0;
    uint32_t n = uint32_t(std::min<uint64_t>(len, f.size - offset));
    if (!self.cache_.read(f.offset + offset, {static_cast<uint8_t *>(out), n})) return -1;
    return n;
}

int DiscVolume::child(const XboxDiscVolume *v, uint32_t id, uint32_t index, XboxDiscEntry *out, char name[256])
{
    const DiscVolume &self = *static_cast<const CVolume *>(v)->self;
    if (id >= self.children_.size() || index >= self.children_[id].size()) return 0;
    uint32_t c = self.children_[id][index];
    const std::string &path = self.index_.all()[c].path;
    size_t slash = path.find_last_of('\\');
    std::string_view leaf = slash == std::string::npos ? std::string_view(path) : std::string_view(path).substr(slash + 1);
    size_t n = std::min<size_t>(leaf.size(), 255);
    std::memcpy(name, leaf.data(), n);
    name[n] = '\0';
    *out = self.entry(c);
    return 1;
}

}  // namespace xbrt
