// D:\ from a disc image, read in place: the fork's XboxDiscVolume (xbox/xbox_frontend.h) over a DiscIndex, with a
// block cache in front of the image. On the console every image read is a request to the USB drive, and a title's
// small reads (headers, tables, a directory sector) cluster inside the same few blocks.
#pragma once

#include "core/disc.h"
#include "core/types.h"

#include "xbox/xbox_frontend.h"

#include <cstdint>
#include <list>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace xbrt {

struct DiscCacheStats {
    uint64_t reads;           // calls to BlockCache::read
    uint64_t block_hits;      // partial blocks served from memory
    uint64_t block_misses;    // partial blocks read from the image, a whole block each
    uint64_t direct_reads;    // runs of whole blocks read straight from the image
    uint64_t direct_bytes;
    uint64_t image_us;        // time spent in the image's read_at, misses and direct runs together
};

// "disc cache 812 reads: 640 block hits, 41 misses, 131 direct (96.4 MB), 1830 ms in the image"
std::string disc_cache_line(const DiscCacheStats &s);

// 64 KiB blocks kept least-recently-used up to 32 MiB. A read covering whole blocks goes straight to the image and is
// not kept: streamed data is read once. Safe from any thread; a miss holds the lock while it reads its block.
class BlockCache {
public:
    static constexpr uint64_t kBlockBytes = 64 * 1024;
    static constexpr size_t kMaxBlocks = 512;

    explicit BlockCache(const Source &image) : image_(image) {}
    bool read(uint64_t offset, std::span<uint8_t> out);
    DiscCacheStats stats() const;

private:
    struct Block {
        uint64_t index;
        std::vector<uint8_t> bytes;   // shorter than a block only at the end of the image
    };
    bool timed_read(uint64_t offset, std::span<uint8_t> out);   // caller holds lock_
    const Block *block(uint64_t index);                          // caller holds lock_

    const Source &image_;
    mutable std::mutex lock_;
    std::list<Block> lru_;   // most recent first
    std::unordered_map<uint64_t, std::list<Block>::iterator> where_;
    DiscCacheStats stats_{};
};

class DiscVolume {
public:
    // Keeps `index` and `image` by reference: both live as long as the session.
    DiscVolume(const DiscIndex &index, const Source &image);
    DiscVolume(const DiscVolume &) = delete;
    DiscVolume &operator=(const DiscVolume &) = delete;

    const XboxDiscVolume *volume() const { return &c_; }
    DiscCacheStats cache_stats() const { return cache_.stats(); }

private:
    struct CVolume : XboxDiscVolume {
        DiscVolume *self;
    };
    static int find(const XboxDiscVolume *v, const char *path, XboxDiscEntry *out);
    static int64_t read(const XboxDiscVolume *v, uint32_t id, uint64_t offset, void *out, uint32_t len);
    static int child(const XboxDiscVolume *v, uint32_t id, uint32_t index, XboxDiscEntry *out, char name[256]);
    XboxDiscEntry entry(uint32_t id) const;

    const DiscIndex &index_;
    BlockCache cache_;
    uint32_t root_;                                 // the root directory's id: one past the last entry of all()
    std::vector<std::vector<uint32_t>> children_;   // by id, in all()'s order
    CVolume c_;
};

}  // namespace xbrt
