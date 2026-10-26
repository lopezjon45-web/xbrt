#include "host/session.h"

#include "core/survey.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <string>

namespace xbrt {

std::string_view boot_stage_name(BootStage s)
{
    switch (s) {
    case BootStage::Reserve: return "reserve";
    case BootStage::Link: return "link";
    case BootStage::OpenDisc: return "disc";
    case BootStage::ReadXbe: return "xbe";
    case BootStage::Analyze: return "analyze";
    case BootStage::MapImage: return "map";
    case BootStage::Kernel: return "kernel";
    case BootStage::Devices: return "devices";
    case BootStage::Start: return "start";
    }
    return "?";
}

std::string xbe_summary_line(const XbeImage &xbe)
{
    uint16_t build = 0;
    for (const XbeLibrary &l : xbe.libraries)
        if (l.name == "XBOXKRNL" || (build == 0 && l.name == "XAPILIB")) build = l.build;
    char buf[160];
    snprintf(buf, sizeof buf, "%s %s XDK %u, %zu imports", xbe.cert.title_name.c_str(), xbe.title_id_hex().c_str(),
             unsigned(build), xbe.imports.size());
    return buf;
}

namespace {

std::string ascii_path(std::u16string_view s)
{
    std::string out;
    for (char16_t c : s) out += c < 128 ? char(c) : '?';
    return out;
}

std::string file_name(std::u16string_view path)
{
    size_t cut = path.find_last_of(u'\\');
    return ascii_path(path.substr(cut == std::u16string_view::npos ? 0 : cut + 1));
}

bool write_atomic(const std::u16string &path, const std::string &text)
{
    std::u16string tmp = path + u".tmp";
    FILE *f = _wfopen(reinterpret_cast<const wchar_t *>(tmp.c_str()), L"wb");
    if (!f) return false;
    bool ok = fwrite(text.data(), 1, text.size(), f) == text.size() && fflush(f) == 0;
    ok = fclose(f) == 0 && ok;
    return ok && _wrename(reinterpret_cast<const wchar_t *>(tmp.c_str()), reinterpret_cast<const wchar_t *>(path.c_str())) == 0;
}

}  // namespace

std::expected<Session, BootError> Session::boot(const LaunchRequest &request, ShellServices &shell)
{
    auto failed = [&shell](BootStage stage, std::string detail) {
        shell.fail(stage, detail);
        return std::unexpected(BootError{stage, std::move(detail)});
    };
    Session s;
    std::string error;
    s.image_ = shell.open_disc(request.disc_path, error);
    if (!s.image_) return failed(BootStage::OpenDisc, ascii_path(request.disc_path) + ": " + error);
    Parsed<DiscIndex> index = DiscIndex::open(*s.image_);
    if (!index) return failed(BootStage::OpenDisc, index.error());
    s.disc_ = std::move(*index);
    char line[256];
    snprintf(line, sizeof line, "disc %s (%zu entries)", ascii_path(request.disc_path).c_str(), s.disc_.all().size());
    shell.stage(BootStage::OpenDisc, line);

    std::string xbe_path = ascii_path(request.xbe);
    std::optional<DiscFile> f = s.disc_.find(xbe_path);
    if (!f) return failed(BootStage::ReadXbe, xbe_path + " is not on the disc");
    Parsed<std::vector<uint8_t>> bytes = s.disc_.read_all(*f);
    if (!bytes) return failed(BootStage::ReadXbe, bytes.error());
    Parsed<XbeImage> xbe = parse_xbe(*bytes);
    if (!xbe) return failed(BootStage::ReadXbe, xbe.error());
    s.xbe_ = std::move(*xbe);
    shell.stage(BootStage::ReadXbe, "xbe " + xbe_summary_line(s.xbe_));
    return s;
}

Parsed<SurveyTotals> run_survey(std::u16string_view folder, ShellServices &shell,
                                const std::function<void(std::string_view)> &progress)
{
    std::string error;
    std::vector<std::u16string> images = shell.list_discs(folder, error);
    if (!error.empty()) return std::unexpected(error);
    if (images.empty()) return std::unexpected("no .iso or .xiso in " + file_name(folder));

    SurveyTotals totals{};
    totals.images = uint32_t(images.size());
    std::vector<SurveyRow> rows;
    std::map<Ordinal, uint32_t> no_bridge;
    std::set<uint16_t> builds;
    std::u16string tsv_path = shell.data_root() + u"\\survey.tsv";
    std::string tsv = tsv_header();
    for (size_t i = 0; i < images.size(); i++) {
        std::string name = file_name(images[i]);
        progress("survey " + std::to_string(i + 1) + "/" + std::to_string(images.size()) + " " + name);
        std::unique_ptr<Source> image = shell.open_disc(images[i], error);
        SurveyRow row;
        if (image) {
            row = survey_disc(*image, name);
        } else {
            row.image_path = name;
            row.reason = error;
        }
        switch (row.verdict) {
        case SurveyVerdict::Ready: totals.ready++; break;
        case SurveyVerdict::NamedGaps: totals.named_gaps++; break;
        case SurveyVerdict::Unparsed: totals.unparsed++; break;
        }
        if (row.verdict != SurveyVerdict::Unparsed) {
            totals.parsed++;
            for (Ordinal o : row.imports_no_bridge) no_bridge[o]++;
            for (const LibraryCoverage &l : row.libraries) builds.insert(l.library.build);
        }
        progress("survey " + std::to_string(i + 1) + "/" + std::to_string(images.size()) + " " + name + ": " +
                 std::string(survey_verdict_name(row.verdict)) + (row.reason.empty() ? "" : " " + row.reason));
        tsv += to_tsv_line(row);
        write_atomic(tsv_path, tsv);
    }
    totals.distinct_builds = uint32_t(builds.size());
    for (const auto &[o, n] : no_bridge) totals.no_bridge_by_titles.push_back({o, n});
    std::sort(totals.no_bridge_by_titles.begin(), totals.no_bridge_by_titles.end(),
              [](const auto &a, const auto &b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
    return totals;
}

}  // namespace xbrt
