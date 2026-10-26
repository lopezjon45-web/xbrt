#pragma once

#include "core/hle_table.h"
#include "core/symbols.h"
#include "core/types.h"
#include "core/xbe.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace xbrt {

enum class SiteKind : uint8_t {
    FsLoad, FsStore, FsStoreEsp, FsStoreImm, FsPush, FsPop, FsMovzxByte, FsCmp, FsCmpRev,
    FsOther,
    Rdtsc,
    Privileged,   // wbinvd, invd, cli, sti: patched to nops; in, out and cr moves stay with the trap router
};

std::string_view site_kind_name(SiteKind k);

struct PatchSite {
    GuestVa at;
    uint8_t length;   // of the original instruction; >= 6 for every Fs* kind with a micro-gate
    SiteKind kind;
    uint8_t reg;      // x86 register number (eax=0 .. edi=7) where the kind has one
    uint32_t disp;
    uint32_t imm;
    std::array<uint8_t, 15> original;
};

// One decoded fs-prefixed instruction's gate form (gates.md section 4). Shared by the code map and the trap router's
// promotion, so both agree on which sites get a micro-gate.
struct FsForm {
    SiteKind kind;
    uint8_t reg;
    uint32_t disp, imm;
};
FsForm classify_fs(std::span<const uint8_t> instruction);

struct CodeMapStats {
    uint32_t bytes_decoded, functions, fs_sites, rdtsc_sites, fs_unpromotable, privileged_sites;
};

// Recursive-descent decode (Zydis, 32-bit) from the entry, every XbSymbolDatabase function and every direct call or
// jump target found, within executable sections only. Instructions it never reaches are left to the trap router.
std::vector<PatchSite> find_sites(const XbeImage &xbe, std::span<const uint8_t> file, const SymbolMap &symbols,
                                  CodeMapStats *stats);

struct ImportSlot {
    GuestVa slot;   // address in the thunk table
    Ordinal ordinal;
};

struct OverrideSite {
    GuestVa entry;   // function entry from XbSymbolDatabase
    HleId id;
};

struct DeviceConstants {
    std::optional<GuestVa> d3d_device_ptr;       // D3D_g_pDevice
    std::optional<uint32_t> d3d_library_build;
};

struct TitlePlan {
    std::array<uint8_t, 32> xbe_sha256;
    std::vector<ImportSlot> imports;
    std::vector<OverrideSite> overrides;
    std::vector<HleId> overrides_missing;   // rows whose symbol the database did not find in this XBE
    std::vector<PatchSite> sites;
    DeviceConstants devices;
    CodeMapStats code_map;
    std::vector<std::string> notes;   // for run.txt and xbrt-inspect: odd sections, unknown libraries, ...
};

TitlePlan plan_title(const XbeImage &xbe, std::span<const uint8_t> file, const SymbolMap &symbols);

// plan.bin: the plan as one little-endian record behind a magic, a format version and the XBE's hash. The reader
// rejects the whole file on any mismatch (magic, version, another XBE's hash, a truncated or overlong record, a
// field out of its range), so a stale cache can never reach apply_sites.
inline constexpr uint32_t kPlanFormatVersion = 1;
std::vector<uint8_t> serialize_plan(const TitlePlan &plan);
Parsed<TitlePlan> deserialize_plan(std::span<const uint8_t> bytes, const std::array<uint8_t, 32> &xbe_sha256);

// Adds sites a run promoted from traps to the plan's sites; a site at an address the plan already has is replaced
// by the learned one, and the result stays sorted by address.
void merge_learned(TitlePlan &plan, std::span<const PatchSite> learned);

}  // namespace xbrt
