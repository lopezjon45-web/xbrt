#include "core/symbols.h"
#include "tests/core/check.h"
#include "tests/core/synth.h"

#include <algorithm>

using namespace synth;

TEST(scan_symbols_finds_nothing_in_a_synthetic_xbe)
{
    std::vector<uint8_t> file = build_xbe();
    xbrt::Parsed<xbrt::XbeImage> x = xbrt::parse_xbe(file);
    CHECK(x.has_value());
    if (!x) return;
    CHECK(xbrt::scan_symbols(*x, file).all().empty());
}

TEST(database_knows_only_libraries_it_has_signatures_for)
{
    for (const char *known : {"D3D8", "XAPILIB", "DSOUND", "XGRAPHC", "XNET"}) {
        bool knows = xbrt::database_knows_library(known);
        CHECK_STR(known + std::string(knows ? " known" : " unknown"), known + std::string(" known"));
    }
    for (const char *unknown : {"D3DX8", "XBOXKRNL", "LIBCMT", ""}) {
        bool knows = xbrt::database_knows_library(unknown);
        CHECK_STR(unknown + std::string(knows ? " known" : " unknown"), unknown + std::string(" unknown"));
    }
}

TEST(symbol_map_finds_names_and_describes_addresses)
{
    std::vector<uint8_t> file = build_xbe();
    xbrt::Parsed<xbrt::XbeImage> x = xbrt::parse_xbe(file);
    CHECK(x.has_value());
    if (!x) return;
    xbrt::SymbolMap map({{"XAPILIB", "Later", {kTextVa + 6}, 5849, true, false},
                         {"D3D8", "D3D_g_pDevice", {kTextVa + 4}, 5849, false, false},
                         {"XAPILIB", "First", {kTextVa}, 5849, true, false},
                         {"XAPILIB", "XapiLastErrorCode_OFFSET", {8}, 5849, false, true},
                         {"XAPILIB", "First", {kTextVa}, 5849, true, false}});
    CHECK_EQ(map.all().size(), size_t(4));
    CHECK(std::is_sorted(map.all().begin(), map.all().end(), [](auto &a, auto &b) { return a.va < b.va; }));

    std::optional<xbrt::Symbol> device = map.find("D3D8", "D3D_g_pDevice");
    CHECK(device && device->va.v == kTextVa + 4 && !device->is_function);
    CHECK(!map.find("XAPILIB", "D3D_g_pDevice"));
    CHECK(!map.find("D3D8", "Missing"));

    std::optional<xbrt::Symbol> near = map.nearest_function({kTextVa + 5});
    CHECK(near && near->name == "First");
    CHECK(!map.nearest_function({kTextVa - 1}));
    CHECK_STR(map.describe({kTextVa + 5}, *x), "XAPILIB First+0x5");
    CHECK_STR(map.describe({kTextVa + 8}, *x), "XAPILIB Later+0x2");
    CHECK_STR(map.describe({kRdataVa + 0x10}, *x), ".rdata+0x10");
    CHECK_STR(map.describe({0x500000}, *x), "0x00500000");
    CHECK_EQ(map.functions_in("XAPILIB"), 2u);
    CHECK_EQ(map.functions_in("D3D8"), 0u);
}
