#include "core/plan.h"
#include "tests/core/check.h"
#include "tests/core/synth.h"

#include <algorithm>
#include <iterator>

using namespace synth;
using xbrt::SiteKind;

namespace {

constexpr uint8_t kCode[] = {
    0x64, 0xA1, 0x00, 0x00, 0x00, 0x00,
    0x64, 0x8B, 0x0D, 0x28, 0x00, 0x00, 0x00,
    0x64, 0x89, 0x25, 0x00, 0x00, 0x00, 0x00,
    0x64, 0xA3, 0x04, 0x00, 0x00, 0x00,
    0x64, 0xC7, 0x05, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
    0x64, 0xFF, 0x35, 0x00, 0x00, 0x00, 0x00,
    0x64, 0x8F, 0x05, 0x00, 0x00, 0x00, 0x00,
    0x64, 0x0F, 0xB6, 0x05, 0x24, 0x00, 0x00, 0x00,
    0x64, 0x3B, 0x35, 0x00, 0x00, 0x00, 0x00,
    0x64, 0x39, 0x35, 0x00, 0x00, 0x00, 0x00,
    0x64, 0x8B, 0x01,
    0x0F, 0x31,
    0xE8, 0x1D, 0x00, 0x00, 0x00,
    0x74, 0x0B,
    0xFA,
    0xC3,
    0x0F, 0x31, 0x0F, 0x08, 0xC3,
    0xCC, 0xCC, 0xCC, 0xCC,
    0xFB, 0xC3,
    0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
    0x0F, 0x09,
    0x64, 0x90,
    0xE8, 0x87, 0x0F, 0x00, 0x00,
    0xC3,
};

struct Expected {
    uint32_t off;
    uint8_t length;
    SiteKind kind;
    uint8_t reg;
    uint32_t disp, imm;
};

constexpr Expected kReached[] = {
    {0x00, 6, SiteKind::FsLoad, 0, 0, 0},       {0x06, 7, SiteKind::FsLoad, 1, 0x28, 0},
    {0x0D, 7, SiteKind::FsStoreEsp, 4, 0, 0},   {0x14, 6, SiteKind::FsStore, 0, 4, 0},
    {0x1A, 11, SiteKind::FsStoreImm, 0, 0, 0xFFFFFFFF},
    {0x25, 7, SiteKind::FsPush, 0, 0, 0},       {0x2C, 7, SiteKind::FsPop, 0, 0, 0},
    {0x33, 8, SiteKind::FsMovzxByte, 0, 0x24, 0},
    {0x3B, 7, SiteKind::FsCmp, 6, 0, 0},        {0x42, 7, SiteKind::FsCmpRev, 6, 0, 0},
    {0x49, 3, SiteKind::FsOther, 0, 0, 0},      {0x4C, 2, SiteKind::Rdtsc, 0, 0, 0},
    {0x55, 1, SiteKind::Privileged, 0, 0, 0},          {0x60, 1, SiteKind::Privileged, 0, 0, 0},
    {0x70, 2, SiteKind::Privileged, 0, 0, 0},
};

xbrt::XbeImage parse(const std::vector<uint8_t> &file)
{
    xbrt::Parsed<xbrt::XbeImage> x = xbrt::parse_xbe(file);
    CHECK(x.has_value());
    return x ? *x : xbrt::XbeImage{};
}

xbrt::Symbol function(const char *library, const char *name, uint32_t va)
{
    return {library, name, {va}, 5849, true, false};
}

}  // namespace

TEST(find_sites_classifies_every_reachable_fs_rdtsc_and_nop_form)
{
    std::vector<uint8_t> file = with_text(build_xbe(), kCode);
    xbrt::XbeImage x = parse(file);
    xbrt::CodeMapStats st{};
    std::vector<xbrt::PatchSite> sites = xbrt::find_sites(x, file, {}, &st);
    CHECK_EQ(sites.size(), std::size(kReached));
    for (size_t i = 0; i < std::min(sites.size(), std::size(kReached)); i++) {
        const xbrt::PatchSite &s = sites[i];
        const Expected &e = kReached[i];
        CHECK_EQ(s.at.v, kTextVa + e.off);
        CHECK_EQ(s.length, e.length);
        CHECK_STR(std::string(xbrt::site_kind_name(s.kind)), std::string(xbrt::site_kind_name(e.kind)));
        CHECK_EQ(s.reg, e.reg);
        CHECK_EQ(s.disp, e.disp);
        CHECK_EQ(s.imm, e.imm);
        CHECK(std::equal(s.original.begin(), s.original.begin() + s.length, kCode + e.off));
        CHECK(std::all_of(s.original.begin() + s.length, s.original.end(), [](uint8_t b) { return b == 0; }));
    }
    CHECK_EQ(st.bytes_decoded, 0x57u + 2 + 10);
    CHECK_EQ(st.functions, 2u);
    CHECK_EQ(st.fs_sites, 10u);
    CHECK_EQ(st.fs_unpromotable, 1u);
    CHECK_EQ(st.rdtsc_sites, 1u);
    CHECK_EQ(st.privileged_sites, 3u);
}

TEST(find_sites_seeds_from_database_functions_and_decodes_each_byte_once)
{
    std::vector<uint8_t> file = with_text(build_xbe(), kCode);
    xbrt::XbeImage x = parse(file);
    xbrt::SymbolMap symbols({function("XAPILIB", "Unreferenced", kTextVa + 0x57), function("XAPILIB", "Entry", kTextVa),
                             {"XAPILIB", "Var", {kTextVa + 0x5C}, 5849, false, false}});
    xbrt::CodeMapStats st{};
    std::vector<xbrt::PatchSite> sites = xbrt::find_sites(x, file, symbols, &st);
    CHECK_EQ(sites.size(), std::size(kReached) + 2);
    CHECK(std::is_sorted(sites.begin(), sites.end(), [](auto &a, auto &b) { return a.at < b.at; }));
    CHECK(std::adjacent_find(sites.begin(), sites.end(), [](auto &a, auto &b) { return a.at == b.at; }) == sites.end());
    CHECK_EQ(st.bytes_decoded, 0x57u + 2 + 10 + 5);
    CHECK_EQ(st.functions, 3u);
    CHECK_EQ(st.rdtsc_sites, 2u);
    CHECK_EQ(st.privileged_sites, 4u);
}

TEST(find_sites_never_decodes_outside_executable_file_data)
{
    const uint8_t runs_off_the_end[] = {0x90, 0x90, 0x0F};
    std::vector<uint8_t> file = with_text(build_xbe(), runs_off_the_end);
    xbrt::XbeImage x = parse(file);
    xbrt::SymbolMap symbols({function("XAPILIB", "InData", kRdataVa)});
    xbrt::CodeMapStats st{};
    CHECK(xbrt::find_sites(x, file, symbols, &st).empty());
    CHECK_EQ(st.bytes_decoded, 2u);
    CHECK_EQ(st.functions, 1u);
}

TEST(plan_title_places_imports_overrides_and_device_constants)
{
    std::vector<uint8_t> file = with_text(build_xbe(), kCode);
    xbrt::XbeImage x = parse(file);
    xbrt::SymbolMap symbols({function("XAPILIB", "QueryPerformanceCounter", kTextVa + 0x70),
                             {"D3D8", "D3D_g_pDevice", {kRdataVa + 0x80}, 4627, false, false}});
    xbrt::TitlePlan plan = xbrt::plan_title(x, file, symbols);

    CHECK(plan.xbe_sha256 == x.sha256);
    CHECK_EQ(plan.imports.size(), std::size(kOrdinals));
    for (size_t i = 0; i < std::min(plan.imports.size(), std::size(kOrdinals)); i++) {
        CHECK_EQ(plan.imports[i].slot.v, kRdataVa + 4 * uint32_t(i));
        CHECK_EQ(plan.imports[i].ordinal.v, kOrdinals[i]);
    }
    CHECK_EQ(plan.overrides.size(), size_t(1));
    if (!plan.overrides.empty()) {
        CHECK_EQ(plan.overrides[0].entry.v, kTextVa + 0x70);
        CHECK(plan.overrides[0].id == xbrt::HleId::QueryPerformanceCounter);
    }
    CHECK(plan.overrides_missing == (std::vector<xbrt::HleId>{xbrt::HleId::XLaunchNewImageA, xbrt::HleId::XapiBootToDash,
                                                             xbrt::HleId::OutputDebugStringA}));
    CHECK(plan.devices.d3d_device_ptr == xbrt::GuestVa{kRdataVa + 0x80});
    CHECK(plan.devices.d3d_library_build == 4627u);
    CHECK_EQ(plan.sites.size(), std::size(kReached));
    CHECK_EQ(plan.code_map.fs_unpromotable, 1u);
    CHECK_EQ(plan.notes.size(), size_t(1));
    if (!plan.notes.empty()) CHECK_CONTAINS(plan.notes[0], "1 fs sites have no micro-gate");
}

TEST(plan_title_names_a_missing_device_pointer_and_unknown_libraries)
{
    std::vector<uint8_t> file = build_xbe();
    std::memcpy(file.data() + kLibsOff, "D3DX8\0\0\0", 8);
    xbrt::XbeImage x = parse(file);
    xbrt::TitlePlan plan = xbrt::plan_title(x, file, {});
    CHECK(!plan.devices.d3d_device_ptr);
    CHECK(plan.devices.d3d_library_build == 4627u);
    CHECK_EQ(plan.overrides_missing.size(), std::size(xbrt::kHleTable));
    CHECK_EQ(plan.notes.size(), size_t(2));
    if (plan.notes.size() == 2) {
        CHECK_CONTAINS(plan.notes[0], "D3D8: no D3D_g_pDevice");
        CHECK_CONTAINS(plan.notes[1], "library D3DX8 1.0.5849: XbSymbolDatabase has no signatures");
    }
}
