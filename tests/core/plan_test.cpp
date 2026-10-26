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

namespace {

xbrt::TitlePlan full_plan()
{
    std::vector<uint8_t> file = with_text(build_xbe(), kCode);
    xbrt::XbeImage x = parse(file);
    xbrt::SymbolMap symbols({function("XAPILIB", "QueryPerformanceCounter", kTextVa + 0x70),
                             {"D3D8", "D3D_g_pDevice", {kRdataVa + 0x80}, 4627, false, false}});
    return xbrt::plan_title(x, file, symbols);
}

bool same_site(const xbrt::PatchSite &a, const xbrt::PatchSite &b)
{
    return a.at == b.at && a.length == b.length && a.kind == b.kind && a.reg == b.reg && a.disp == b.disp && a.imm == b.imm &&
           a.original == b.original;
}

std::string plan_difference(const xbrt::TitlePlan &a, const xbrt::TitlePlan &b)
{
    if (a.xbe_sha256 != b.xbe_sha256) return "sha256";
    if (a.imports.size() != b.imports.size()) return "imports count";
    for (size_t i = 0; i < a.imports.size(); i++)
        if (a.imports[i].slot != b.imports[i].slot || a.imports[i].ordinal != b.imports[i].ordinal) return "import " + std::to_string(i);
    if (a.overrides.size() != b.overrides.size()) return "overrides count";
    for (size_t i = 0; i < a.overrides.size(); i++)
        if (a.overrides[i].entry != b.overrides[i].entry || a.overrides[i].id != b.overrides[i].id) return "override " + std::to_string(i);
    if (a.overrides_missing != b.overrides_missing) return "overrides_missing";
    if (a.sites.size() != b.sites.size()) return "sites count";
    for (size_t i = 0; i < a.sites.size(); i++)
        if (!same_site(a.sites[i], b.sites[i])) return "site " + std::to_string(i);
    if (a.devices.d3d_device_ptr != b.devices.d3d_device_ptr || a.devices.d3d_library_build != b.devices.d3d_library_build) return "devices";
    const xbrt::CodeMapStats &ca = a.code_map, &cb = b.code_map;
    if (ca.bytes_decoded != cb.bytes_decoded || ca.functions != cb.functions || ca.fs_sites != cb.fs_sites ||
        ca.rdtsc_sites != cb.rdtsc_sites || ca.fs_unpromotable != cb.fs_unpromotable || ca.privileged_sites != cb.privileged_sites)
        return "code_map";
    if (a.notes != b.notes) return "notes";
    return "";
}

}  // namespace

TEST(plan_round_trips_through_plan_bin)
{
    xbrt::TitlePlan plan = full_plan();
    CHECK(!plan.imports.empty() && !plan.overrides.empty() && !plan.overrides_missing.empty() && !plan.sites.empty() &&
          !plan.notes.empty() && plan.devices.d3d_device_ptr.has_value());
    std::vector<uint8_t> bytes = xbrt::serialize_plan(plan);
    xbrt::Parsed<xbrt::TitlePlan> back = xbrt::deserialize_plan(bytes, plan.xbe_sha256);
    CHECK(back.has_value());
    if (back) CHECK_STR(plan_difference(plan, *back), "");

    xbrt::TitlePlan bare{};
    bare.xbe_sha256 = plan.xbe_sha256;
    xbrt::Parsed<xbrt::TitlePlan> bare_back = xbrt::deserialize_plan(xbrt::serialize_plan(bare), bare.xbe_sha256);
    CHECK(bare_back.has_value());
    if (bare_back) CHECK_STR(plan_difference(bare, *bare_back), "");
}

TEST(plan_bin_is_rejected_whole_on_any_mismatch)
{
    xbrt::TitlePlan plan = full_plan();
    std::vector<uint8_t> bytes = xbrt::serialize_plan(plan);

    std::array<uint8_t, 32> other = plan.xbe_sha256;
    other[5] ^= 1;
    xbrt::Parsed<xbrt::TitlePlan> stale = xbrt::deserialize_plan(bytes, other);
    CHECK(!stale.has_value());
    if (!stale) CHECK_CONTAINS(stale.error(), "another XBE");

    std::vector<uint8_t> old = bytes;
    old[8] = uint8_t(xbrt::kPlanFormatVersion + 1);
    xbrt::Parsed<xbrt::TitlePlan> version = xbrt::deserialize_plan(old, plan.xbe_sha256);
    CHECK(!version.has_value());
    if (!version) CHECK_CONTAINS(version.error(), "format");

    std::vector<uint8_t> magic = bytes;
    magic[0] = 'Y';
    CHECK(!xbrt::deserialize_plan(magic, plan.xbe_sha256).has_value());

    for (size_t cut : {size_t(0), size_t(7), size_t(40), bytes.size() / 2, bytes.size() - 1}) {
        std::vector<uint8_t> truncated(bytes.begin(), bytes.begin() + cut);
        xbrt::Parsed<xbrt::TitlePlan> t = xbrt::deserialize_plan(truncated, plan.xbe_sha256);
        CHECK(!t.has_value());
    }

    std::vector<uint8_t> padded = bytes;
    padded.push_back(0);
    xbrt::Parsed<xbrt::TitlePlan> trailing = xbrt::deserialize_plan(padded, plan.xbe_sha256);
    CHECK(!trailing.has_value());
    if (!trailing) CHECK_CONTAINS(trailing.error(), "after the record");

    xbrt::TitlePlan bad_kind = plan;
    bad_kind.sites[0].kind = xbrt::SiteKind(uint8_t(xbrt::SiteKind::Privileged) + 1);
    CHECK(!xbrt::deserialize_plan(xbrt::serialize_plan(bad_kind), plan.xbe_sha256).has_value());
    xbrt::TitlePlan bad_length = plan;
    bad_length.sites[0].length = 16;
    CHECK(!xbrt::deserialize_plan(xbrt::serialize_plan(bad_length), plan.xbe_sha256).has_value());
}

TEST(merge_learned_adds_new_sites_and_replaces_planned_ones_at_the_same_address)
{
    xbrt::TitlePlan plan = full_plan();
    size_t before = plan.sites.size();
    xbrt::PatchSite replaced = plan.sites[3];
    replaced.kind = xbrt::SiteKind::FsOther;
    xbrt::PatchSite first{{kTextVa - 0x10}, 7, xbrt::SiteKind::FsLoad, 1, 0x18, 0, {}};
    xbrt::PatchSite last{{kTextVa + 0x1000}, 2, xbrt::SiteKind::Rdtsc, 0, 0, 0, {}};
    const xbrt::PatchSite learned[] = {last, replaced, first};
    xbrt::merge_learned(plan, learned);
    CHECK_EQ(plan.sites.size(), before + 2);
    CHECK(std::is_sorted(plan.sites.begin(), plan.sites.end(), [](auto &a, auto &b) { return a.at < b.at; }));
    CHECK(same_site(plan.sites.front(), first));
    CHECK(same_site(plan.sites.back(), last));
    auto at = std::find_if(plan.sites.begin(), plan.sites.end(), [&](auto &s) { return s.at == replaced.at; });
    CHECK(at != plan.sites.end() && at->kind == xbrt::SiteKind::FsOther);
    CHECK(std::adjacent_find(plan.sites.begin(), plan.sites.end(), [](auto &a, auto &b) { return a.at == b.at; }) == plan.sites.end());
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
