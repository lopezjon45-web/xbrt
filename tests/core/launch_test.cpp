#include "core/launch.h"
#include "tests/core/check.h"

using xbrt::QueryPairs;

namespace {

std::string narrow(std::u16string_view s)
{
    std::string out;
    for (char16_t c : s) out += c < 0x80 ? char(c) : '?';
    return out;
}

std::string error_of(const QueryPairs &pairs)
{
    xbrt::Parsed<xbrt::LaunchRequest> r = xbrt::parse_launch(pairs);
    return r ? std::string("(parsed)") : r.error();
}

const std::u16string kGame = u"E:\\Xbox\\Some Game (USA).iso";
const std::u16string kBack = u"decomp:?from=xboxrt";

}  // namespace

TEST(parse_launch_reads_the_hubs_disc_link)
{
    xbrt::Parsed<xbrt::LaunchRequest> r =
        xbrt::parse_launch({{u"disc", kGame}, {u"launchOnExit", kBack}, {u"vault", u"ignored"}});
    CHECK(r.has_value());
    if (!r) return;
    CHECK(r->disc_path == kGame);
    CHECK_STR(narrow(r->xbe), "\\default.xbe");
    CHECK(r->return_uri == kBack);
    CHECK(r->survey_folder.empty());
    CHECK(!r->show_counters);
    CHECK(r->trace == xbrt::Trace::Off);
}

TEST(parse_launch_reads_the_optional_values)
{
    xbrt::Parsed<xbrt::LaunchRequest> r =
        xbrt::parse_launch({{u"launchOnExit", kBack}, {u"disc", u"e:\\Xbox\\Other.XISO"}, {u"xbe", u"\\bin\\Setup.XBE"},
                            {u"fps", u"1"}, {u"trace", u"kernel"}});
    CHECK(r.has_value());
    if (!r) return;
    CHECK_STR(narrow(r->disc_path), "e:\\Xbox\\Other.XISO");
    CHECK_STR(narrow(r->xbe), "\\bin\\Setup.XBE");
    CHECK(r->show_counters);
    CHECK(r->trace == xbrt::Trace::Kernel);
    xbrt::Parsed<xbrt::LaunchRequest> traps =
        xbrt::parse_launch({{u"launchOnExit", kBack}, {u"disc", kGame}, {u"trace", u"traps"}, {u"fps", u"0"}});
    CHECK(traps && traps->trace == xbrt::Trace::Traps && !traps->show_counters);
}

TEST(parse_launch_reads_a_survey_link)
{
    xbrt::Parsed<xbrt::LaunchRequest> r = xbrt::parse_launch({{u"survey", u"E:\\Xbox"}, {u"launchOnExit", u"decomp:"}});
    CHECK(r.has_value());
    if (!r) return;
    CHECK_STR(narrow(r->survey_folder), "E:\\Xbox");
    CHECK(r->disc_path.empty());
}

TEST(parse_launch_names_the_field_it_rejects)
{
    CHECK_CONTAINS(error_of({{u"disc", kGame}}), "launchOnExit");
    CHECK_CONTAINS(error_of({{u"disc", kGame}, {u"launchOnExit", u""}}), "launchOnExit");
    CHECK_CONTAINS(error_of({{u"launchOnExit", kBack}}), "disc: missing");
    CHECK_CONTAINS(error_of({{u"launchOnExit", kBack}, {u"disc", kGame}, {u"survey", u"E:\\Xbox"}}), "disc and survey");
    CHECK_CONTAINS(error_of({{u"launchOnExit", kBack}, {u"disc", u"Xbox\\game.iso"}}), "disc: \"Xbox\\game.iso\" is not an absolute");
    CHECK_CONTAINS(error_of({{u"launchOnExit", kBack}, {u"disc", u"\\\\server\\game.iso"}}), "disc:");
    CHECK_CONTAINS(error_of({{u"launchOnExit", kBack}, {u"disc", u"E:\\Xbox\\game.zip"}}), "not an .iso or .xiso");
    CHECK_CONTAINS(error_of({{u"launchOnExit", kBack}, {u"disc", u"E:\\Xbox\\game.iso.txt"}}), "disc:");
    CHECK_CONTAINS(error_of({{u"launchOnExit", kBack}, {u"survey", u"Xbox"}}), "survey:");
    CHECK_CONTAINS(error_of({{u"launchOnExit", kBack}, {u"disc", kGame}, {u"xbe", u"default.xbe"}}), "xbe:");
    CHECK_CONTAINS(error_of({{u"launchOnExit", kBack}, {u"disc", kGame}, {u"xbe", u"\\default.exe"}}), "xbe:");
    CHECK_CONTAINS(error_of({{u"launchOnExit", kBack}, {u"disc", kGame}, {u"xbe", u"\\.xbe"}}), "xbe:");
    CHECK_CONTAINS(error_of({{u"launchOnExit", kBack}, {u"disc", kGame}, {u"fps", u"yes"}}), "fps:");
    CHECK_CONTAINS(error_of({{u"launchOnExit", kBack}, {u"disc", kGame}, {u"trace", u"all"}}), "trace:");
}

TEST(return_link_appends_the_outcome_to_a_bare_scheme)
{
    CHECK_STR(narrow(xbrt::return_link(u"decomp:", "ok", "", "")), "decomp:?result=ok");
    CHECK_STR(narrow(xbrt::return_link(u"decomp:?", "ok", "", "")), "decomp:?result=ok");
}

TEST(return_link_joins_a_return_uri_that_already_carries_a_query)
{
    CHECK_STR(narrow(xbrt::return_link(kBack, "ok", "", "")), "decomp:?from=xboxrt&result=ok");
    CHECK_STR(narrow(xbrt::return_link(kBack, "error", "Feature", "XLaunchNewImage(\\setup.xbe)")),
              "decomp:?from=xboxrt&result=error&kind=Feature&missing=XLaunchNewImage%28%5Csetup.xbe%29");
}

TEST(return_link_escapes_every_byte_outside_the_rfc3986_unreserved_set)
{
    CHECK_STR(narrow(xbrt::return_link(u"decomp:", "error", "Kernel", "AZaz09-._~ !&=?%+/'\xc3\xa9")),
              "decomp:?result=error&kind=Kernel&missing=AZaz09-._~%20%21%26%3D%3F%25%2B%2F%27%C3%A9");
}
