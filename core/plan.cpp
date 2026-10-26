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

struct FsForm {
    SiteKind kind;
    uint8_t reg;
    uint32_t disp, imm;
};

uint32_t le32(const uint8_t *p)
{
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

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

}  // namespace xbrt
