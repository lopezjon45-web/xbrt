#include "core/disc.h"
#include "core/xbe.h"
#include "tests/core/check.h"
#include "tests/core/synth.h"

using namespace synth;

namespace {

std::string error_of(const Xiso &img)
{
    xbrt::Parsed<xbrt::DiscIndex> d = xbrt::DiscIndex::open(img);
    return d ? std::string("(opened)") : d.error();
}

void lists_every_entry(uint64_t base)
{
    Xiso img = game_like(base);
    xbrt::Parsed<xbrt::DiscIndex> disc = xbrt::DiscIndex::open(img);
    CHECK(disc.has_value());
    if (!disc) {
        std::printf("  error: %s\n", disc.error().c_str());
        return;
    }
    struct Want {
        const char *path;
        uint32_t sector;
        uint64_t size;
        bool directory;
    };
    const Want want[] = {{"a.txt", 41, 5, false},          {"default.xbe", 40, 100, false},
                         {"empty.dat", 0, 0, false},       {"media", 34, 4096, true},
                         {"media\\Deep", 36, 2048, true},  {"media\\Deep\\z.bin", 45, 1, false},
                         {"media\\x.big", 42, 3000, false}, {"media\\y.big", 44, 10, false}};
    const std::vector<xbrt::DiscFile> &all = disc->all();
    CHECK_EQ(all.size(), std::size(want));
    for (size_t i = 0; i < all.size() && i < std::size(want); i++) {
        CHECK_STR(all[i].path, want[i].path);
        CHECK_EQ(all[i].offset, base + want[i].sector * kSector);
        CHECK_EQ(all[i].size, want[i].size);
        CHECK(all[i].directory == want[i].directory);
    }
}

}  // namespace

TEST(disc_index_lists_files_and_directories_of_a_bare_partition_in_lower_cased_path_order)
{
    lists_every_entry(0);
}

TEST(disc_index_finds_the_game_partition_after_a_redump_video_partition)
{
    lists_every_entry(0x18300000);
}

TEST(disc_index_find_is_case_insensitive_and_takes_a_leading_backslash)
{
    Xiso img = game_like(0);
    xbrt::Parsed<xbrt::DiscIndex> disc = xbrt::DiscIndex::open(img);
    CHECK(disc.has_value());
    if (!disc) return;
    std::optional<xbrt::DiscFile> xbe = disc->find("\\DEFAULT.XBE");
    CHECK(xbe && xbe->path == "default.xbe" && xbe->offset == 40 * kSector);
    CHECK(disc->find("default.xbe").has_value());
    std::optional<xbrt::DiscFile> nested = disc->find("MEDIA\\deep\\Z.BIN");
    CHECK(nested && nested->path == "media\\Deep\\z.bin");
    CHECK(disc->find("media")->directory);
    CHECK(!disc->find("D:\\default.xbe"));
    CHECK(!disc->find("default.xb"));
    CHECK(!disc->find("media\\"));
    CHECK(!disc->find(""));
}

TEST(disc_index_lists_one_directory)
{
    Xiso img = game_like(0);
    xbrt::Parsed<xbrt::DiscIndex> disc = xbrt::DiscIndex::open(img);
    CHECK(disc.has_value());
    if (!disc) return;
    auto paths = [](const std::vector<xbrt::DiscFile> &v) {
        std::string s;
        for (const xbrt::DiscFile &f : v) s += f.path + ";";
        return s;
    };
    CHECK_STR(paths(disc->list("")), "a.txt;default.xbe;empty.dat;media;");
    CHECK_STR(paths(disc->list("\\")), "a.txt;default.xbe;empty.dat;media;");
    CHECK_STR(paths(disc->list("MEDIA")), "media\\Deep;media\\x.big;media\\y.big;");
    CHECK_STR(paths(disc->list("\\media\\deep\\")), "media\\Deep\\z.bin;");
    CHECK_STR(paths(disc->list("nowhere")), "");
}

TEST(disc_index_reads_inside_a_file_only)
{
    Xiso img = game_like(0);
    img.file(41, {'h', 'e', 'l', 'l', 'o', '!', '!'});
    xbrt::Parsed<xbrt::DiscIndex> disc = xbrt::DiscIndex::open(img);
    CHECK(disc.has_value());
    if (!disc) return;
    xbrt::DiscFile a = *disc->find("a.txt");
    xbrt::Parsed<std::vector<uint8_t>> all = disc->read_all(a);
    CHECK(all && std::string(all->begin(), all->end()) == "hello");
    uint8_t two[2];
    CHECK(disc->read(a, 3, two) && two[0] == 'l' && two[1] == 'o');
    CHECK(!disc->read(a, 4, two));
    CHECK(!disc->read(a, 6, std::span<uint8_t>(two, 0)));
    CHECK(disc->read(a, 5, std::span<uint8_t>(two, 0)));
    xbrt::Parsed<std::vector<uint8_t>> dir = disc->read_all(*disc->find("media"));
    CHECK(!dir);
    CHECK_CONTAINS(dir.error(), "is a directory");
}

TEST(disc_index_rejects_bad_images)
{
    Xiso loop = game_like(0);
    loop.directory(36, {{{"back", 33, 2048, kDirAttr}}});
    CHECK_CONTAINS(error_of(loop), "reached twice");

    Xiso past = game_like(0);
    past.directory(36, {{{"z.bin", 45, 1u << 30}}});
    CHECK_CONTAINS(error_of(past), "\"media\\Deep\\z.bin\" lies outside the image");

    Xiso dotdot = game_like(0);
    dotdot.directory(36, {{{"..", 37, 2048, kDirAttr}}});
    CHECK_CONTAINS(error_of(dotdot), "not a plain file name");

    Xiso slash = game_like(0);
    slash.directory(36, {{{"a\\b", 45, 1}}});
    CHECK_CONTAINS(error_of(slash), "not a plain file name");

    Xiso blank(0);
    blank.at(40, kSector);
    CHECK_CONTAINS(error_of(blank), "not an Xbox disc image");

    Xiso overrun = game_like(0);
    overrun.directory(34, {{{"x.big", 42, 3000}, {"deep", 36, 20, kDirAttr}}, {{"y.big", 44, 10}}});
    overrun.directory(36, {{{"zzz.bin", 45, 1}}});
    CHECK_CONTAINS(error_of(overrun), "runs past the end of its sector");

    Xiso cut = game_like(0);
    cut.directory(34, {{{"x.big", 42, 3000}, {"deep", 90, 2048, kDirAttr}}, {{"y.big", 44, 10}}});
    cut.reported_size = cut.size() + 64 * kSector;
    CHECK_CONTAINS(error_of(cut), "read failed in directory \"media\\deep\\\"");

    Xiso twins = game_like(0);
    twins.directory(36, {{{"z.bin", 45, 1}, {"Z.BIN", 45, 1}}});
    CHECK_CONTAINS(error_of(twins), "two entries are named");
}

TEST(disc_index_hands_parse_xbe_the_default_xbe)
{
    Xiso img = game_like(0x18300000);
    std::vector<uint8_t> xbe = build_xbe();
    img.directory(33, {{{"default.xbe", 50, uint32_t(xbe.size())}, {"media", 34, 4096, kDirAttr}}});
    img.file(50, xbe);
    xbrt::Parsed<xbrt::DiscIndex> disc = xbrt::DiscIndex::open(img);
    CHECK(disc.has_value());
    if (!disc) return;
    std::optional<xbrt::DiscFile> f = disc->find("\\default.xbe");
    CHECK(f.has_value());
    if (!f) return;
    xbrt::Parsed<std::vector<uint8_t>> bytes = disc->read_all(*f);
    CHECK(bytes && *bytes == xbe);
    xbrt::Parsed<xbrt::XbeImage> parsed = xbrt::parse_xbe(*bytes);
    CHECK(parsed && parsed->title_id_hex() == "FFFF0001" && parsed->imports.size() == std::size(kOrdinals));
}
