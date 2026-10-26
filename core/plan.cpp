#include "core/plan.h"

#include <Zydis/Decoder.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace xbrt {

std::string_view site_kind_name(SiteKind k)
{
    switch (k) {
    case SiteKind::FsLoad: return "fs-load";
    case SiteKind::FsStore: return "fs-store";
    case SiteKind::FsStoreEsp: return "fs-store-esp";
    case SiteKind::FsStoreImm: return "fs-store-imm";
    case SiteKind::FsPush: return "fs-push";
    case SiteKind::FsPop: return "fs-pop";
    case SiteKind::FsMovzxByte: return "fs-movzx-byte";
    case SiteKind::FsCmp: return "fs-cmp";
    case SiteKind::FsCmpRev: return "fs-cmp-rev";
    case SiteKind::FsOther: return "fs-other";
    case SiteKind::Rdtsc: return "rdtsc";
    case SiteKind::Privileged: return "privileged";
    }
    return "?";
}

namespace {

uint32_t le32(const uint8_t *p)
{
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}


std::span<const uint8_t> code_at(const XbeImage &xbe, std::span<const uint8_t> file, GuestVa va)
{
    const XbeSection *s = xbe.section_at(va);
    if (!s || !s->executable || s->inserted_file) return {};
    uint32_t off = va.v - s->va.v, end = std::min(s->raw_size, s->virtual_size);
    if (off >= end) return {};
    return file.subspan(s->raw_offset + off, end - off);
}

bool ends_flow(const ZydisDecodedInstruction &in)
{
    switch (in.meta.category) {
    case ZYDIS_CATEGORY_RET:
    case ZYDIS_CATEGORY_UNCOND_BR: return true;
    default: break;
    }
    return in.mnemonic == ZYDIS_MNEMONIC_INT3 || in.mnemonic == ZYDIS_MNEMONIC_HLT || in.mnemonic == ZYDIS_MNEMONIC_UD2;
}

bool is_nop_kind(ZydisMnemonic m)
{
    return m == ZYDIS_MNEMONIC_WBINVD || m == ZYDIS_MNEMONIC_INVD || m == ZYDIS_MNEMONIC_CLI || m == ZYDIS_MNEMONIC_STI;
}

}  // namespace

FsForm classify_fs(std::span<const uint8_t> b)
{
    const FsForm other{SiteKind::FsOther, 0, 0, 0};
    if (b[0] != 0x64) return other;
    if (b[1] == 0xA1) return {SiteKind::FsLoad, 0, le32(&b[2]), 0};
    if (b[1] == 0xA3) return {SiteKind::FsStore, 0, le32(&b[2]), 0};
    if (b.size() < 7) return other;
    if (b[1] == 0x0F) {
        bool absolute = b[2] == 0xB6 && (b[3] & 0xC7) == 0x05;
        return absolute ? FsForm{SiteKind::FsMovzxByte, uint8_t(b[3] >> 3 & 7), le32(&b[4]), 0} : other;
    }
    if ((b[2] & 0xC7) != 0x05) return other;
    uint8_t reg = b[2] >> 3 & 7;
    uint32_t disp = le32(&b[3]);
    switch (b[1]) {
    case 0x8B: return {SiteKind::FsLoad, reg, disp, 0};
    case 0x89: return {reg == 4 ? SiteKind::FsStoreEsp : SiteKind::FsStore, reg, disp, 0};
    case 0x3B: return {SiteKind::FsCmp, reg, disp, 0};
    case 0x39: return {SiteKind::FsCmpRev, reg, disp, 0};
    case 0xC7: return reg == 0 ? FsForm{SiteKind::FsStoreImm, 0, disp, le32(&b[7])} : other;
    case 0xFF: return reg == 6 ? FsForm{SiteKind::FsPush, 0, disp, 0} : other;
    case 0x8F: return reg == 0 ? FsForm{SiteKind::FsPop, 0, disp, 0} : other;
    }
    return other;
}

std::vector<PatchSite> find_sites(const XbeImage &xbe, std::span<const uint8_t> file, const SymbolMap &symbols,
                                  CodeMapStats *stats)
{
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_STACK_WIDTH_32);

    struct Root {
        GuestVa at;
        bool function;
    };
    std::vector<Root> work{{xbe.entry, true}};
    for (const Symbol &s : symbols.all())
        if (s.is_function && !s.is_offset) work.push_back({s.va, true});

    std::vector<bool> decoded(xbe.image_size);
    std::vector<PatchSite> sites;
    CodeMapStats st{};
    while (!work.empty()) {
        Root root = work.back();
        work.pop_back();
        for (GuestVa at = root.at;;) {
            std::span<const uint8_t> code = code_at(xbe, file, at);
            if (code.empty() || decoded[at.v - xbe.base.v]) break;
            ZydisDecodedInstruction in;
            if (!ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(&decoder, nullptr, code.data(), code.size(), &in))) break;
            if (at == root.at && root.function) st.functions++;
            for (uint32_t i = 0; i < in.length; i++) decoded[at.v - xbe.base.v + i] = true;
            st.bytes_decoded += in.length;

            std::optional<FsForm> form;
            if (in.attributes & ZYDIS_ATTRIB_HAS_SEGMENT_FS) {
                form = classify_fs(code.first(in.length));
                if (form->kind == SiteKind::FsOther) {
                    form->disp = uint32_t(in.raw.disp.value);
                    st.fs_unpromotable++;
                } else {
                    st.fs_sites++;
                }
            } else if (in.mnemonic == ZYDIS_MNEMONIC_RDTSC) {
                form = FsForm{SiteKind::Rdtsc, 0, 0, 0};
                st.rdtsc_sites++;
            } else if (is_nop_kind(in.mnemonic)) {
                form = FsForm{SiteKind::Privileged, 0, 0, 0};
                st.privileged_sites++;
            }
            if (form) {
                PatchSite site{at, in.length, form->kind, form->reg, form->disp, form->imm, {}};
                std::copy_n(code.begin(), in.length, site.original.begin());
                sites.push_back(site);
            }

            if (in.raw.imm[0].is_relative)
                work.push_back({at + in.length + uint32_t(in.raw.imm[0].value.s),
                                in.meta.category == ZYDIS_CATEGORY_CALL});
            if (ends_flow(in)) break;
            at = at + in.length;
        }
    }
    std::sort(sites.begin(), sites.end(), [](const PatchSite &a, const PatchSite &b) { return a.at < b.at; });
    if (stats) *stats = st;
    return sites;
}

TitlePlan plan_title(const XbeImage &xbe, std::span<const uint8_t> file, const SymbolMap &symbols)
{
    TitlePlan plan{};
    plan.xbe_sha256 = xbe.sha256;
    for (size_t i = 0; i < xbe.imports.size(); i++)
        plan.imports.push_back({xbe.thunk_table + uint32_t(4 * i), xbe.imports[i]});

    for (const HleRow &row : kHleTable) {
        std::optional<Symbol> s = symbols.find(row.library, row.symbol);
        if (s && s->is_function)
            plan.overrides.push_back({s->va, row.id});
        else
            plan.overrides_missing.push_back(row.id);
    }

    char note[160];
    // A title links D3D8 or, built with link-time code generation, D3D8LTCG; the database names symbols after it.
    for (std::string_view d3d : {"D3D8", "D3D8LTCG"}) {
        auto lib = std::find_if(xbe.libraries.begin(), xbe.libraries.end(),
                                [&](const XbeLibrary &l) { return l.name == d3d; });
        if (lib == xbe.libraries.end()) continue;
        plan.devices.d3d_library_build = lib->build;
        std::optional<Symbol> device = symbols.find(d3d, "D3D_g_pDevice");
        if (device && !device->is_function && !device->is_offset) {
            plan.devices.d3d_device_ptr = device->va;
        } else {
            std::snprintf(note, sizeof note, "%s: no D3D_g_pDevice symbol, so the fence mirror has no device pointer",
                          lib->name.c_str());
            plan.notes.push_back(note);
        }
        break;
    }

    for (const XbeLibrary &l : xbe.libraries) {
        if (database_knows_library(l.name)) continue;
        std::snprintf(note, sizeof note, "library %s %u.%u.%u: XbSymbolDatabase has no signatures for it",
                      l.name.c_str(), l.major, l.minor, l.build);
        plan.notes.push_back(note);
    }

    plan.sites = find_sites(xbe, file, symbols, &plan.code_map);
    if (plan.code_map.fs_unpromotable) {
        std::snprintf(note, sizeof note, "code map: %u fs sites have no micro-gate and stay with the trap router",
                      plan.code_map.fs_unpromotable);
        plan.notes.push_back(note);
    }
    return plan;
}

namespace {

constexpr char kPlanMagic[8] = {'X', 'B', 'R', 'T', 'P', 'L', 'A', 'N'};
constexpr uint32_t kMaxPlanCount = 1u << 20;   // more entries than any XBE can hold: a count above it is corruption

struct Writer {
    std::vector<uint8_t> out;
    void u8(uint8_t v) { out.push_back(v); }
    void u16(uint16_t v) { bytes(&v, 2); }
    void u32(uint32_t v) { bytes(&v, 4); }
    void bytes(const void *p, size_t n) { out.insert(out.end(), static_cast<const uint8_t *>(p), static_cast<const uint8_t *>(p) + n); }
    void str(const std::string &s)
    {
        u32(uint32_t(s.size()));
        bytes(s.data(), s.size());
    }
};

// Every read is bounds-checked; the first overrun sets `error` and every later read returns zero, so a caller can
// read a whole record and test once at the end.
struct Reader {
    std::span<const uint8_t> in;
    size_t at = 0;
    std::string error;

    bool take(void *p, size_t n)
    {
        if (!error.empty()) return false;
        if (n > in.size() - at) {
            error = "plan.bin: truncated";
            return false;
        }
        std::memcpy(p, in.data() + at, n);
        at += n;
        return true;
    }
    uint8_t u8()
    {
        uint8_t v = 0;
        take(&v, 1);
        return v;
    }
    uint16_t u16()
    {
        uint16_t v = 0;
        take(&v, 2);
        return v;
    }
    uint32_t u32()
    {
        uint32_t v = 0;
        take(&v, 4);
        return v;
    }
    uint32_t count()
    {
        uint32_t n = u32();
        if (error.empty() && n > kMaxPlanCount) error = "plan.bin: a count of " + std::to_string(n) + " entries";
        return error.empty() ? n : 0;
    }
    std::string str()
    {
        uint32_t n = count();
        std::string s(n, '\0');
        take(s.data(), n);
        return s;
    }
};

}  // namespace

std::vector<uint8_t> serialize_plan(const TitlePlan &plan)
{
    Writer w;
    w.bytes(kPlanMagic, sizeof kPlanMagic);
    w.u32(kPlanFormatVersion);
    w.bytes(plan.xbe_sha256.data(), plan.xbe_sha256.size());
    w.u32(uint32_t(plan.imports.size()));
    for (const ImportSlot &i : plan.imports) {
        w.u32(i.slot.v);
        w.u16(i.ordinal.v);
    }
    w.u32(uint32_t(plan.overrides.size()));
    for (const OverrideSite &o : plan.overrides) {
        w.u32(o.entry.v);
        w.u16(uint16_t(o.id));
    }
    w.u32(uint32_t(plan.overrides_missing.size()));
    for (HleId id : plan.overrides_missing) w.u16(uint16_t(id));
    w.u32(uint32_t(plan.sites.size()));
    for (const PatchSite &s : plan.sites) {
        w.u32(s.at.v);
        w.u8(s.length);
        w.u8(uint8_t(s.kind));
        w.u8(s.reg);
        w.u32(s.disp);
        w.u32(s.imm);
        w.bytes(s.original.data(), s.original.size());
    }
    w.u8(plan.devices.d3d_device_ptr.has_value());
    w.u32(plan.devices.d3d_device_ptr.value_or(GuestVa{}).v);
    w.u8(plan.devices.d3d_library_build.has_value());
    w.u32(plan.devices.d3d_library_build.value_or(0));
    const CodeMapStats &c = plan.code_map;
    for (uint32_t v : {c.bytes_decoded, c.functions, c.fs_sites, c.rdtsc_sites, c.fs_unpromotable, c.privileged_sites}) w.u32(v);
    w.u32(uint32_t(plan.notes.size()));
    for (const std::string &n : plan.notes) w.str(n);
    return std::move(w.out);
}

Parsed<TitlePlan> deserialize_plan(std::span<const uint8_t> bytes, const std::array<uint8_t, 32> &xbe_sha256)
{
    Reader r{bytes};
    char magic[sizeof kPlanMagic] = {};
    r.take(magic, sizeof magic);
    if (r.error.empty() && std::memcmp(magic, kPlanMagic, sizeof magic) != 0) return std::unexpected("plan.bin: not a plan file");
    uint32_t version = r.u32();
    if (r.error.empty() && version != kPlanFormatVersion)
        return std::unexpected("plan.bin: format " + std::to_string(version) + ", this build reads " + std::to_string(kPlanFormatVersion));
    TitlePlan plan{};
    r.take(plan.xbe_sha256.data(), plan.xbe_sha256.size());
    if (r.error.empty() && plan.xbe_sha256 != xbe_sha256) return std::unexpected("plan.bin: the plan of another XBE");

    for (uint32_t n = r.count(); n && r.error.empty(); n--) {
        ImportSlot i{{r.u32()}, {r.u16()}};
        plan.imports.push_back(i);
    }
    for (uint32_t n = r.count(); n && r.error.empty(); n--) {
        OverrideSite o{{r.u32()}, HleId(r.u16())};
        if (o.id >= HleId::Count_) return std::unexpected("plan.bin: unknown override id");
        plan.overrides.push_back(o);
    }
    for (uint32_t n = r.count(); n && r.error.empty(); n--) {
        HleId id = HleId(r.u16());
        if (id >= HleId::Count_) return std::unexpected("plan.bin: unknown override id");
        plan.overrides_missing.push_back(id);
    }
    for (uint32_t n = r.count(); n && r.error.empty(); n--) {
        PatchSite s{};
        s.at = {r.u32()};
        s.length = r.u8();
        s.kind = SiteKind(r.u8());
        s.reg = r.u8();
        s.disp = r.u32();
        s.imm = r.u32();
        r.take(s.original.data(), s.original.size());
        if (r.error.empty() && (s.kind > SiteKind::Privileged || s.length == 0 || s.length > s.original.size() || s.reg > 7))
            return std::unexpected("plan.bin: a site out of range");
        plan.sites.push_back(s);
    }
    bool has_ptr = r.u8() != 0;
    uint32_t ptr = r.u32();
    bool has_build = r.u8() != 0;
    uint32_t build = r.u32();
    if (has_ptr) plan.devices.d3d_device_ptr = GuestVa{ptr};
    if (has_build) plan.devices.d3d_library_build = build;
    CodeMapStats &c = plan.code_map;
    for (uint32_t *v : {&c.bytes_decoded, &c.functions, &c.fs_sites, &c.rdtsc_sites, &c.fs_unpromotable, &c.privileged_sites}) *v = r.u32();
    for (uint32_t n = r.count(); n && r.error.empty(); n--) plan.notes.push_back(r.str());
    if (!r.error.empty()) return std::unexpected(r.error);
    if (r.at != bytes.size()) return std::unexpected("plan.bin: " + std::to_string(bytes.size() - r.at) + " bytes after the record");
    return plan;
}

void merge_learned(TitlePlan &plan, std::span<const PatchSite> learned)
{
    for (const PatchSite &site : learned) {
        auto at = std::lower_bound(plan.sites.begin(), plan.sites.end(), site,
                                   [](const PatchSite &a, const PatchSite &b) { return a.at < b.at; });
        if (at != plan.sites.end() && at->at == site.at)
            *at = site;
        else
            plan.sites.insert(at, site);
    }
}

}  // namespace xbrt
