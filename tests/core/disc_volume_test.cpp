#include "core/disc_volume.h"
#include "tests/core/check.h"
#include "tests/core/synth.h"

#include <random>

using namespace synth;

namespace {

// byte i of the image is a function of i, so every read can be checked anywhere; counts what reached the image.
struct Pattern final : xbrt::Source {
    uint64_t bytes;
    mutable uint64_t calls = 0, read_bytes = 0;
    explicit Pattern(uint64_t n) : bytes(n) {}
    static uint8_t at(uint64_t i) { return uint8_t(i * 131 + (i >> 16)); }
    uint64_t size() const override { return bytes; }
    bool read_at(uint64_t offset, std::span<uint8_t> out) const override
    {
        calls++;
        read_bytes += out.size();
        if (offset > bytes || out.size() > bytes - offset) return false;
        for (size_t i = 0; i < out.size(); i++) out[i] = at(offset + i);
        return true;
    }
};

bool matches(const std::vector<uint8_t> &got, uint64_t offset)
{
    for (size_t i = 0; i < got.size(); i++)
        if (got[i] != Pattern::at(offset + i)) return false;
    return true;
}

constexpr uint64_t kBlock = xbrt::BlockCache::kBlockBytes;

std::string children(const XboxDiscVolume *v, uint32_t id)
{
    std::string s;
    XboxDiscEntry e;
    char name[256];
    for (uint32_t i = 0; v->child(v, id, i, &e, name); i++) s += std::string(name) + ((e.attributes & 0x10) ? "/;" : ";");
    return s;
}

}  // namespace

TEST(block_cache_serves_small_reads_in_one_block_from_one_image_read)
{
    Pattern image(4 * kBlock);
    xbrt::BlockCache cache(image);
    std::vector<uint8_t> a(100), b(300);
    CHECK(cache.read(kBlock + 10, a) && matches(a, kBlock + 10));
    CHECK(cache.read(kBlock + 2000, b) && matches(b, kBlock + 2000));
    CHECK_EQ(image.calls, 1u);
    xbrt::DiscCacheStats s = cache.stats();
    CHECK_EQ(s.block_misses, 1u);
    CHECK_EQ(s.block_hits, 1u);
}

TEST(block_cache_reads_whole_blocks_straight_from_the_image_and_keeps_the_partial_ends)
{
    Pattern image(8 * kBlock);
    xbrt::BlockCache cache(image);
    std::vector<uint8_t> big(size_t(3 * kBlock));
    CHECK(cache.read(kBlock / 2, big) && matches(big, kBlock / 2));
    xbrt::DiscCacheStats s = cache.stats();
    CHECK_EQ(s.block_misses, 2u);
    CHECK_EQ(s.direct_reads, 1u);
    CHECK_EQ(s.direct_bytes, 2 * kBlock);
    std::vector<uint8_t> inside(64);
    CHECK(cache.read(2 * kBlock, inside) && matches(inside, 2 * kBlock));
    CHECK_EQ(cache.stats().block_misses, 3u);
    CHECK(cache.read(kBlock / 2 + 7, inside) && matches(inside, kBlock / 2 + 7));
    CHECK_EQ(cache.stats().block_hits, 1u);
}

TEST(block_cache_evicts_the_least_recently_used_block)
{
    constexpr uint64_t kMax = xbrt::BlockCache::kMaxBlocks;
    Pattern image((kMax + 2) * kBlock);
    xbrt::BlockCache cache(image);
    std::vector<uint8_t> one(1);
    for (uint64_t b = 0; b < kMax; b++) CHECK(cache.read(b * kBlock + 1, one));
    CHECK(cache.read(0 * kBlock + 2, one));
    CHECK(cache.read(kMax * kBlock + 1, one));
    uint64_t before = image.calls;
    CHECK(cache.read(0 * kBlock + 3, one) && one[0] == Pattern::at(3));
    CHECK_EQ(image.calls, before);
    CHECK(cache.read(1 * kBlock + 3, one) && one[0] == Pattern::at(kBlock + 3));
    CHECK_EQ(image.calls, before + 1);
}

TEST(block_cache_returns_the_image_bytes_for_any_read_and_refuses_past_the_end)
{
    Pattern image(5 * kBlock + 1234);
    xbrt::BlockCache cache(image);
    std::mt19937 rng(7);
    bool all_match = true;
    for (int i = 0; i < 400; i++) {
        uint64_t len = rng() % (3 * kBlock);
        uint64_t offset = rng() % (image.size() - len + 1);
        std::vector<uint8_t> out(static_cast<size_t>(len));
        all_match = all_match && cache.read(offset, out) && matches(out, offset);
    }
    CHECK(all_match);
    std::vector<uint8_t> tail(1234);
    CHECK(cache.read(5 * kBlock, tail) && matches(tail, 5 * kBlock));
    std::vector<uint8_t> over(2);
    CHECK(!cache.read(image.size() - 1, over));
}

TEST(disc_volume_finds_lists_and_reads_like_the_disc_index)
{
    Xiso img = game_like(0);
    img.file(42, std::vector<uint8_t>(3000, 0));
    for (uint32_t i = 0; i < 3000; i++) img.at(42, 3000)[i] = uint8_t(i * 7);
    xbrt::Parsed<xbrt::DiscIndex> disc = xbrt::DiscIndex::open(img);
    CHECK(disc.has_value());
    if (!disc) return;
    xbrt::DiscVolume volume(*disc, img);
    const XboxDiscVolume *v = volume.volume();

    XboxDiscEntry root, media, deep, big, z;
    CHECK(v->find(v, "", &root) && (root.attributes & 0x10));
    CHECK_STR(children(v, root.id), "a.txt;default.xbe;empty.dat;media/;");
    CHECK(v->find(v, "MEDIA", &media) && (media.attributes & 0x10) && media.size == 0);
    CHECK_STR(children(v, media.id), "Deep/;x.big;y.big;");
    CHECK(v->find(v, "media\\deep", &deep));
    CHECK_STR(children(v, deep.id), "z.bin;");
    CHECK(v->find(v, "media\\DEEP\\Z.BIN", &z) && z.size == 1 && !(z.attributes & 0x10));
    CHECK(!v->find(v, "media\\missing.bin", &z));
    CHECK(!v->find(v, "nowhere\\z.bin", &z));

    CHECK(v->find(v, "media\\x.big", &big) && big.size == 3000);
    uint8_t buf[16];
    CHECK_EQ(v->read(v, big.id, 2990, buf, sizeof buf), 10);
    CHECK(buf[0] == uint8_t(2990 * 7) && buf[9] == uint8_t(2999 * 7));
    CHECK_EQ(v->read(v, big.id, 3000, buf, sizeof buf), 0);
    CHECK_EQ(v->read(v, media.id, 0, buf, sizeof buf), -1);
    CHECK_EQ(v->image_bytes, img.size());
}
