#include "core/disc.h"
#include "core/survey.h"
#include "tests/core/check.h"
#include "tests/core/synth.h"

#include <algorithm>

using namespace synth;

namespace {

constexpr xbrt::Ordinal kFull{1}, kNone{367}, kStub{7};

Xiso two_xbe_disc(std::u16string_view title = u"SYNTH")
{
    std::vector<uint8_t> xbe = build_xbe(kRetail, title);
    const xbrt::Ordinal imports[] = {kFull, kNone, kStub};
    for (size_t i = 0; i < std::size(imports); i++) put32(xbe, kThunkFileOff + i * 4, imports[i].v | 0x80000000);
    Xiso img(0);
    img.descriptor(33, 2048);
    img.directory(33, {{{"default.xbe", 40, uint32_t(xbe.size())}, {"bonus", 34, 2048, kDirAttr}}});
    img.directory(34, {{{"Extra.XBE", 50, 4}, {"readme.txt", 51, 3}}});
    img.file(40, xbe);
    img.at(51, kSector);
    return img;
}

size_t columns(const std::string &line)
{
    return size_t(std::count(line.begin(), line.end(), '\t')) + 1;
}

}  // namespace

TEST(survey_disc_lists_every_xbe_and_splits_imports_by_bridge_status)
{
    CHECK(xbrt::bridge_status(kFull) == xbrt::BridgeStatus::Full);
    CHECK(xbrt::bridge_status(kNone) == xbrt::BridgeStatus::None);
    CHECK(xbrt::bridge_status(kStub) == xbrt::BridgeStatus::Stub);

    Xiso img = two_xbe_disc();
    xbrt::SurveyRow row = xbrt::survey_disc(img, "synth.xiso");
    CHECK_STR(row.image_path, "synth.xiso");
    CHECK_STR(row.title_name, "SYNTH");
    CHECK_STR(row.title_id, "FFFF0001");
    CHECK_EQ(row.entry.v, kTextVa);
    CHECK(row.xbes == (std::vector<std::string>{"bonus\\Extra.XBE", "default.xbe"}));
    CHECK_EQ(row.imports_total, 3u);
    CHECK(row.imports_no_bridge == std::vector<xbrt::Ordinal>{kNone});
    CHECK(row.imports_stub == std::vector<xbrt::Ordinal>{kStub});
    CHECK_EQ(row.libraries.size(), 2u);
    CHECK(row.verdict == xbrt::SurveyVerdict::NamedGaps);
    CHECK_CONTAINS(row.reason, "imports with no bridge 1 (367 )");
    CHECK_CONTAINS(row.reason, "stub imports 1 (7 DbgLoadImageSymbols)");
}

TEST(survey_disc_on_a_disc_the_lister_rejects_is_unparsed_with_the_listers_reason)
{
    Xiso img(0);
    img.at(40, kSector);
    xbrt::Parsed<xbrt::DiscIndex> disc = xbrt::DiscIndex::open(img);
    CHECK(!disc.has_value());
    if (disc) return;
    xbrt::SurveyRow row = xbrt::survey_disc(img, "bad.iso");
    CHECK(row.verdict == xbrt::SurveyVerdict::Unparsed);
    CHECK_STR(row.reason, disc.error());
    CHECK(row.title_id.empty());
}

TEST(survey_disc_names_an_xbe_that_does_not_parse_and_still_lists_the_xbes)
{
    Xiso img = two_xbe_disc();
    img.at(40, 4)[0] = 'Q';
    xbrt::SurveyRow row = xbrt::survey_disc(img, "synth.xiso");
    CHECK(row.verdict == xbrt::SurveyVerdict::Unparsed);
    CHECK(row.reason.starts_with("default.xbe: "));
    CHECK_EQ(row.xbes.size(), 2u);
}

TEST(to_tsv_writes_one_cell_per_header_column_and_turns_a_tab_in_a_title_into_a_space)
{
    Xiso img = two_xbe_disc(u"TAB\tTITLE");
    Xiso bad(0);
    bad.at(40, kSector);
    std::string tsv = xbrt::to_tsv({xbrt::survey_disc(img, "synth.xiso"), xbrt::survey_disc(bad, "bad.iso")});

    std::vector<std::string> lines;
    for (size_t at = 0, nl; (nl = tsv.find('\n', at)) != std::string::npos; at = nl + 1)
        lines.push_back(tsv.substr(at, nl - at));
    CHECK_EQ(lines.size(), 3u);
    if (lines.size() != 3) return;
    CHECK_STR(lines[0] + "\n", xbrt::tsv_header());
    CHECK(lines[0].starts_with("image\ttitle\ttitle_id\tentry\tverdict\treason\txbes\tlibraries\t"));
    CHECK_EQ(columns(lines[0]), 19u);
    CHECK_EQ(columns(lines[1]), 19u);
    CHECK_EQ(columns(lines[2]), 19u);
    CHECK(lines[1].starts_with("synth.xiso\tTAB TITLE\tFFFF0001\t0x11000\tnamed-gaps\t"));
    CHECK_CONTAINS(lines[1], "\tbonus\\Extra.XBE;default.xbe\tXAPILIB:5849:");
    CHECK_CONTAINS(lines[1], "\t3\t367=\t7=DbgLoadImageSymbols\t");
    CHECK(lines[2].starts_with("bad.iso\t\t\t\tunparsed\t"));
    CHECK(lines[2].ends_with(std::string(13, '\t')));
}
