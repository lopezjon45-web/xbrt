#include "core/survey.h"

#include "core/disc.h"
#include "core/hle_table.h"
#include "core/symbols.h"
#include "core/xbe.h"

#include <cstdio>
#include <iterator>

namespace xbrt {

#include "core/gen/bridge_status.inc"

namespace {

constexpr size_t kReasonNames = 3;

constexpr std::string_view kColumns[] = {
    "image", "title", "title_id", "entry", "verdict", "reason", "xbes", "libraries", "imports_total",
    "imports_no_bridge", "imports_stub", "fs_sites", "fs_unpromotable", "rdtsc_sites", "nop_sites", "bytes_decoded",
    "functions", "overrides_placed", "calls_launch_new_image",
};

bool ends_with_ci(std::string_view s, std::string_view suffix)
{
    if (s.size() < suffix.size()) return false;
    for (size_t i = 0; i < suffix.size(); i++) {
        char c = s[s.size() - suffix.size() + i];
        if ((c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c) != suffix[i]) return false;
    }
    return true;
}

std::string ordinal_label(Ordinal o) { return std::to_string(o.v) + " " + std::string(kernel_export_name(o)); }

std::string first_names(const std::vector<Ordinal> &ordinals)
{
    std::string out;
    for (size_t i = 0; i < ordinals.size() && i < kReasonNames; i++)
        out += (i ? ", " : "") + ordinal_label(ordinals[i]);
    if (ordinals.size() > kReasonNames) out += ", +" + std::to_string(ordinals.size() - kReasonNames) + " more";
    return out;
}

std::string gap_summary(const SurveyRow &row)
{
    std::string out;
    auto add = [&](const std::string &part) { out += (out.empty() ? "" : "; ") + part; };
    if (!row.imports_no_bridge.empty())
        add("imports with no bridge " + std::to_string(row.imports_no_bridge.size()) + " (" +
            first_names(row.imports_no_bridge) + ")");
    if (!row.imports_stub.empty())
        add("stub imports " + std::to_string(row.imports_stub.size()) + " (" + first_names(row.imports_stub) + ")");
    if (row.code_map.fs_unpromotable) add("unpromotable fs forms " + std::to_string(row.code_map.fs_unpromotable));
    return out;
}

void survey_into(SurveyRow &row, std::span<const uint8_t> file)
{
    Parsed<XbeImage> parsed = parse_xbe(file);
    if (!parsed) {
        row.reason = parsed.error();
        return;
    }
    const XbeImage &xbe = *parsed;
    SymbolMap symbols = scan_symbols(xbe, file);
    TitlePlan plan = plan_title(xbe, file, symbols);

    row.title_name = xbe.cert.title_name;
    row.title_id = xbe.title_id_hex();
    row.entry = xbe.entry;
    for (const XbeLibrary &lib : xbe.libraries)
        row.libraries.push_back({lib, database_knows_library(lib.name), symbols.functions_in(lib.name)});
    row.imports_total = uint32_t(xbe.imports.size());
    for (Ordinal o : xbe.imports) {
        switch (bridge_status(o)) {
        case BridgeStatus::None: row.imports_no_bridge.push_back(o); break;
        case BridgeStatus::Stub: row.imports_stub.push_back(o); break;
        case BridgeStatus::Full:
        case BridgeStatus::Partial:
        case BridgeStatus::Data: break;
        }
    }
    row.code_map = plan.code_map;
    for (const OverrideSite &o : plan.overrides) row.overrides_placed.push_back(o.id);
    const HleRow &launch = kHleTable[size_t(HleId::XLaunchNewImageA)];
    row.calls_launch_new_image = symbols.find(launch.library, launch.symbol).has_value();
    row.verdict = row.imports_no_bridge.empty() && row.code_map.fs_unpromotable == 0 ? SurveyVerdict::Ready
                                                                                      : SurveyVerdict::NamedGaps;
    row.reason = gap_summary(row);
}

std::string cell(std::string_view v)
{
    std::string out(v);
    for (char &c : out)
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return out;
}

template <class T, class F>
std::string joined(const std::vector<T> &items, F &&format)
{
    std::string out;
    for (size_t i = 0; i < items.size(); i++) out += (i ? ";" : "") + format(items[i]);
    return out;
}

std::string ordinal_pairs(const std::vector<Ordinal> &ordinals)
{
    return joined(ordinals, [](Ordinal o) { return std::to_string(o.v) + "=" + std::string(kernel_export_name(o)); });
}

}  // namespace

BridgeStatus bridge_status(Ordinal o)
{
    return o.v < std::size(kBridgeStatus) ? kBridgeStatus[o.v] : BridgeStatus::None;
}

std::string_view survey_verdict_name(SurveyVerdict v)
{
    switch (v) {
    case SurveyVerdict::Ready: return "ready";
    case SurveyVerdict::NamedGaps: return "named-gaps";
    case SurveyVerdict::Unparsed: return "unparsed";
    }
    return "?";
}

SurveyRow survey_disc(const Source &image, std::string_view path)
{
    SurveyRow row;
    row.image_path = std::string(path);
    Parsed<DiscIndex> disc = DiscIndex::open(image);
    if (!disc) {
        row.reason = disc.error();
        return row;
    }
    for (const DiscFile &f : disc->all())
        if (!f.directory && ends_with_ci(f.path, ".xbe")) row.xbes.push_back(f.path);
    std::optional<DiscFile> xbe = disc->find("default.xbe");
    if (!xbe) {
        row.reason = "no default.xbe on the disc";
        return row;
    }
    Parsed<std::vector<uint8_t>> bytes = disc->read_all(*xbe);
    if (!bytes) {
        row.reason = bytes.error();
        return row;
    }
    survey_into(row, *bytes);
    if (row.verdict == SurveyVerdict::Unparsed) row.reason = "default.xbe: " + row.reason;
    return row;
}

SurveyRow survey_xbe(std::span<const uint8_t> file, std::string_view path)
{
    SurveyRow row;
    row.image_path = std::string(path);
    survey_into(row, file);
    return row;
}

std::string tsv_header()
{
    std::string out;
    for (size_t i = 0; i < std::size(kColumns); i++) out += (i ? "\t" : "") + std::string(kColumns[i]);
    return out + "\n";
}

// An Unparsed row leaves every column the XBE would have given empty, so no reader mistakes a missing count for 0.
std::string to_tsv_line(const SurveyRow &row)
{
    const bool parsed = row.verdict != SurveyVerdict::Unparsed;
    auto num = [&](uint64_t v) { return parsed ? std::to_string(v) : std::string(); };
    char entry[16] = "";
    if (parsed) std::snprintf(entry, sizeof entry, "0x%X", row.entry.v);
    const CodeMapStats &m = row.code_map;
    const std::string cells[] = {
        row.image_path,
        row.title_name,
        row.title_id,
        entry,
        std::string(survey_verdict_name(row.verdict)),
        row.reason,
        joined(row.xbes, [](const std::string &x) { return x; }),
        joined(row.libraries,
               [](const LibraryCoverage &l) {
                   return l.library.name + ":" + std::to_string(l.library.build) + ":" +
                          (l.database_has_signatures ? "known" : "unknown") + ":" + std::to_string(l.functions_named);
               }),
        num(row.imports_total),
        ordinal_pairs(row.imports_no_bridge),
        ordinal_pairs(row.imports_stub),
        num(m.fs_sites),
        num(m.fs_unpromotable),
        num(m.rdtsc_sites),
        num(m.nop_sites),
        num(m.bytes_decoded),
        num(m.functions),
        joined(row.overrides_placed, [](HleId id) { return std::string(kHleTable[size_t(id)].symbol); }),
        parsed ? (row.calls_launch_new_image ? "1" : "0") : "",
    };
    static_assert(sizeof cells / sizeof cells[0] == std::size(kColumns), "one cell per column");
    std::string out;
    for (size_t i = 0; i < std::size(cells); i++) out += (i ? "\t" : "") + cell(cells[i]);
    return out + "\n";
}

std::string to_tsv(const std::vector<SurveyRow> &rows)
{
    std::string out = tsv_header();
    for (const SurveyRow &r : rows) out += to_tsv_line(r);
    return out;
}

}  // namespace xbrt
