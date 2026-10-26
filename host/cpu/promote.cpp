#include "host/cpu/cpu_internal.h"

#include <cstring>

namespace xbrt {

namespace detail {

namespace {

uint8_t *host(uint32_t va) { return reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(va)); }

uint32_t le32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

uint32_t variant_for(const VariantKey &key, PatchLockHeld held)
{
    CpuState &s = *g_cpu;
    auto it = s.variant_entries.find(key);
    if (it != s.variant_entries.end()) return it->second;
    std::vector<uint8_t> body = emit_micro_gate(key.kind, key.reg, key.disp, key.imm);
    if (body.empty()) return 0;
    uint32_t at = (s.chunk + s.variants_cursor + 15) & ~15u;
    if (at + body.size() > s.chunk + kFarPtrOffset) return 0;
    memcpy(host(at), body.data(), body.size());
    FlushInstructionCache(GetCurrentProcess(), host(at), body.size());
    s.variants_cursor = at + uint32_t(body.size()) - s.chunk;
    uint32_t entry = write_far_pointer(at, held);
    if (entry) s.variant_entries[key] = entry;
    return entry;
}

// gates.md 6: hlt first, so a thread reaching the site parks in row 1; the tail; then the first byte and a flush.
void write_site(uint32_t at, const std::vector<uint8_t> &bytes)
{
    CpuState &s = *g_cpu;
    uint8_t *p = host(at);
    MEMORY_BASIC_INFORMATION info{};
    ULONG old = 0;
    if (VirtualQuery(p, &info, sizeof info) && !(info.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)))
        VirtualProtectFromApp(p, bytes.size(), PAGE_EXECUTE_READWRITE, &old);
    s.patching_site = at;
    p[0] = kHlt;
    FlushProcessWriteBuffers();
    memcpy(p + 1, bytes.data() + 1, bytes.size() - 1);
    FlushProcessWriteBuffers();
    p[0] = bytes[0];
    FlushInstructionCache(GetCurrentProcess(), p, bytes.size());
    s.patching_site = 0;
}

}  // namespace

// The byte forms core/plan.cpp's code map classifies, so a learned site and a planned site name the same kind.
FsForm classify_fs(std::span<const uint8_t> b)
{
    const FsForm other{SiteKind::FsOther, 0, 0, 0};
    if (b.size() < 6 || b[0] != 0x64) return other;
    if (b[1] == 0xA1) return {SiteKind::FsLoad, 0, le32(&b[2]), 0};
    if (b[1] == 0xA3) return {SiteKind::FsStore, 0, le32(&b[2]), 0};
    if (b.size() < 7) return other;
    if (b[1] == 0x0F) {
        bool absolute = b[2] == 0xB6 && (b[3] & 0xC7) == 0x05 && b.size() >= 8;
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
    case 0xC7: return reg == 0 && b.size() >= 11 ? FsForm{SiteKind::FsStoreImm, 0, disp, le32(&b[7])} : other;
    case 0xFF: return reg == 6 ? FsForm{SiteKind::FsPush, 0, disp, 0} : other;
    case 0x8F: return reg == 0 ? FsForm{SiteKind::FsPop, 0, disp, 0} : other;
    }
    return other;
}

bool promote(const PatchSite &site, bool learned)
{
    CpuState &s = *g_cpu;
    std::vector<uint8_t> bytes;
    std::lock_guard<std::mutex> hold(s.patch_lock);
    switch (site.kind) {
    case SiteKind::Rdtsc:
        if (site.length != 2) return false;
        bytes = {kRdtscTrap[0], kRdtscTrap[1]};
        break;
    case SiteKind::Privileged: bytes.assign(site.length, 0x90); break;
    case SiteKind::FsOther: return false;
    default: {
        if (site.length < kPatchLength || !has_micro_gate(site.kind, site.reg)) return false;
        uint32_t entry = variant_for({site.kind, site.reg, site.disp, site.imm}, hold);
        if (!entry) return false;
        bytes = emit_patch(entry, site.length);
        break;
    }
    }
    write_site(site.at.v, bytes);
    if (site.kind != SiteKind::Rdtsc) s.promoted.insert(site.at.v);
    if (site.kind == SiteKind::Rdtsc) {
        std::lock_guard<std::mutex> routes(s.routes_lock);
        s.rdtsc_sites.insert(site.at.v);
    }
    if (learned) {
        s.learned.push_back(site);
        s.sites_promoted_learned++;
    } else {
        s.sites_promoted_eager++;
    }
    return true;
}

}  // namespace detail

using namespace detail;

std::vector<PatchSite> Cpu::apply_sites(std::span<const PatchSite> sites)
{
    std::vector<PatchSite> skipped;
    for (const PatchSite &site : sites) {
        if (site.length == 0 || site.length > site.original.size() || memcmp(host(site.at.v), site.original.data(), site.length) != 0) {
            skipped.push_back(site);
            continue;
        }
        if (!promote(site, false)) skipped.push_back(site);
    }
    return skipped;
}

void Cpu::apply_overrides(std::span<const OverrideSite> overrides)
{
    for (const OverrideSite &o : overrides) {
        uint32_t target = gate_for(HostCallId::override_(o.id)).v;
        uint32_t rel = target - (o.entry.v + 5);
        std::vector<uint8_t> jump = {0xE9, uint8_t(rel), uint8_t(rel >> 8), uint8_t(rel >> 16), uint8_t(rel >> 24)};
        std::lock_guard<std::mutex> hold(g_cpu->patch_lock);
        write_site(o.entry.v, jump);
    }
}

}  // namespace xbrt
