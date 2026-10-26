// xbrt-inspect <default.xbe | game.iso> [--imports] [--plan [--sites]]: the facts the runtime reads from an XBE and
// the plan it would make, on the Mac or in CI.
// xbrt-inspect --survey <dir | image | xbe>... [--out survey.tsv]: the survey's rows for local images and XBEs.
// Reads only; Jon's game data never leaves the machine it is on.
#include "core/disc.h"
#include "core/plan.h"
#include "core/survey.h"
#include "core/symbols.h"
#include "core/xbe.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

class FileSource final : public xbrt::Source {
public:
    explicit FileSource(const fs::path &path) : in_(path, std::ios::binary)
    {
        std::error_code ec;
        size_ = fs::file_size(path, ec);
        if (ec) in_.close();
    }
    bool is_open() const { return in_.is_open(); }
    bool read_at(uint64_t offset, std::span<uint8_t> out) const override
    {
        if (offset > size_ || out.size() > size_ - offset) return false;
        in_.clear();
        in_.seekg(std::streamoff(offset));
        in_.read(reinterpret_cast<char *>(out.data()), std::streamsize(out.size()));
        return bool(in_);
    }
    uint64_t size() const override { return size_; }

private:
    mutable std::ifstream in_;
    uint64_t size_ = 0;
};

std::optional<std::vector<uint8_t>> read_whole(const FileSource &src)
{
    std::vector<uint8_t> bytes(size_t(src.size()));
    if (!src.read_at(0, bytes)) return std::nullopt;
    return bytes;
}

bool ends_with_ci(const std::string &s, const char *suffix)
{
    size_t n = std::strlen(suffix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; i++)
        if (std::tolower((unsigned char)s[s.size() - n + i]) != suffix[i]) return false;
    return true;
}

bool is_disc_image(const std::string &path) { return ends_with_ci(path, ".iso") || ends_with_ci(path, ".xiso"); }

int fail(const std::string &what)
{
    std::fprintf(stderr, "xbrt-inspect: %s\n", what.c_str());
    return 1;
}

std::string_view hle_name(xbrt::HleId id) { return xbrt::kHleTable[size_t(id)].symbol; }

void print_plan(const xbrt::XbeImage &x, std::span<const uint8_t> file, bool sites)
{
    xbrt::SymbolMap symbols = xbrt::scan_symbols(x, file);
    std::map<std::string, uint32_t> variables;
    for (const xbrt::Symbol &s : symbols.all()) variables[s.library] += !s.is_function;
    std::printf("symbols: %zu\n", symbols.all().size());
    for (const auto &[library, n] : variables)
        std::printf("  %-8s functions %u variables %u\n", library.c_str(), symbols.functions_in(library), n);

    xbrt::TitlePlan plan = xbrt::plan_title(x, file, symbols);
    const xbrt::CodeMapStats &m = plan.code_map;
    std::printf("code map: bytes_decoded %u functions %u fs_sites %u rdtsc_sites %u fs_unpromotable %u nop_sites %u\n",
                m.bytes_decoded, m.functions, m.fs_sites, m.rdtsc_sites, m.fs_unpromotable, m.nop_sites);
    std::printf("import slots: %zu\n", plan.imports.size());
    std::printf("overrides: %zu\n", plan.overrides.size());
    for (const xbrt::OverrideSite &o : plan.overrides)
        std::printf("  0x%08X %s\n", o.entry.v, std::string(hle_name(o.id)).c_str());
    std::printf("overrides missing: %zu\n", plan.overrides_missing.size());
    for (xbrt::HleId id : plan.overrides_missing) std::printf("  %s\n", std::string(hle_name(id)).c_str());
    if (plan.devices.d3d_device_ptr) std::printf("d3d_device_ptr 0x%08X\n", plan.devices.d3d_device_ptr->v);
    if (plan.devices.d3d_library_build) std::printf("d3d_library_build %u\n", *plan.devices.d3d_library_build);
    for (const std::string &n : plan.notes) std::printf("note: %s\n", n.c_str());
    if (sites)
        for (const xbrt::PatchSite &s : plan.sites)
            std::printf("site 0x%08X %u %s %u 0x%X\n", s.at.v, s.length,
                        std::string(xbrt::site_kind_name(s.kind)).c_str(), s.reg, s.disp);
}

xbrt::SurveyRow survey_file(const fs::path &path)
{
    std::string name = path.string();
    FileSource src(path);
    if (!src.is_open()) {
        xbrt::SurveyRow row;
        row.image_path = name;
        row.reason = "cannot open the file";
        return row;
    }
    if (is_disc_image(name)) return xbrt::survey_disc(src, name);
    std::optional<std::vector<uint8_t>> bytes = read_whole(src);
    if (!bytes) {
        xbrt::SurveyRow row;
        row.image_path = name;
        row.reason = "read failed";
        return row;
    }
    return xbrt::survey_xbe(*bytes, name);
}

bool survey_kind(const fs::path &p)
{
    std::string s = p.string();
    return is_disc_image(s) || ends_with_ci(s, ".xbe");
}

std::optional<std::string> collect(const fs::path &arg, std::vector<fs::path> &out)
{
    std::error_code ec;
    if (fs::is_regular_file(arg, ec)) {
        if (!survey_kind(arg)) return "not an .iso, .xiso or .xbe: " + arg.string();
        out.push_back(arg);
        return std::nullopt;
    }
    if (!fs::is_directory(arg, ec)) return "no such file or directory: " + arg.string();
    std::vector<fs::path> found;
    fs::recursive_directory_iterator it(arg, fs::directory_options::skip_permission_denied, ec), end;
    if (ec) return "cannot list " + arg.string() + ": " + ec.message();
    for (; it != end; it.increment(ec)) {
        if (ec) return "cannot list " + arg.string() + ": " + ec.message();
        if (it.depth() >= 1) it.disable_recursion_pending();
        if (it->is_regular_file(ec) && survey_kind(it->path())) found.push_back(it->path());
    }
    std::sort(found.begin(), found.end());
    out.insert(out.end(), found.begin(), found.end());
    return std::nullopt;
}

int survey(int argc, char **argv)
{
    std::vector<fs::path> inputs;
    std::optional<std::string> out_path;
    for (int i = 2; i < argc; i++) {
        if (std::strcmp(argv[i], "--out") == 0) {
            if (++i == argc) return fail("--out needs a file name");
            out_path = argv[i];
            continue;
        }
        if (std::optional<std::string> error = collect(argv[i], inputs)) return fail(*error);
    }
    if (inputs.empty()) return fail("no .iso, .xiso or .xbe found in the arguments");

    std::vector<xbrt::SurveyRow> rows;
    size_t counts[3] = {};
    for (const fs::path &p : inputs) {
        rows.push_back(survey_file(p));
        counts[size_t(rows.back().verdict)]++;
    }
    std::string tsv = xbrt::to_tsv(rows);
    if (out_path) {
        std::ofstream out(*out_path, std::ios::binary);
        if (!out.write(tsv.data(), std::streamsize(tsv.size()))) return fail("cannot write " + *out_path);
    } else {
        std::fwrite(tsv.data(), 1, tsv.size(), stdout);
    }
    std::fprintf(stderr, "xbrt-inspect: %zu rows: %zu ready, %zu named-gaps, %zu unparsed\n", rows.size(),
                 counts[size_t(xbrt::SurveyVerdict::Ready)], counts[size_t(xbrt::SurveyVerdict::NamedGaps)],
                 counts[size_t(xbrt::SurveyVerdict::Unparsed)]);
    return 0;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc >= 2 && std::strcmp(argv[1], "--survey") == 0) return survey(argc, argv);
    if (argc < 2)
        return fail("usage: xbrt-inspect <default.xbe | game.iso> [--imports] [--plan [--sites]]\n"
                    "       xbrt-inspect --survey <dir | image | xbe>... [--out survey.tsv]");
    bool imports = false, plan = false, sites = false;
    for (int i = 2; i < argc; i++) {
        if (std::strcmp(argv[i], "--imports") == 0)
            imports = true;
        else if (std::strcmp(argv[i], "--plan") == 0)
            plan = true;
        else if (std::strcmp(argv[i], "--sites") == 0)
            sites = true;
        else
            return fail(std::string("unknown option ") + argv[i]);
    }
    if (sites && !plan) return fail("--sites needs --plan");
    FileSource src(argv[1]);
    if (!src.is_open()) return fail(std::string("cannot open ") + argv[1]);

    std::vector<uint8_t> xbe_bytes;
    std::string path = argv[1];
    if (is_disc_image(path)) {
        xbrt::Parsed<xbrt::DiscIndex> disc = xbrt::DiscIndex::open(src);
        if (!disc) return fail(disc.error());
        std::printf("disc: %zu entries\n", disc->all().size());
        std::optional<xbrt::DiscFile> f = disc->find("default.xbe");
        if (!f) return fail("no default.xbe on the disc");
        xbrt::Parsed<std::vector<uint8_t>> bytes = disc->read_all(*f);
        if (!bytes) return fail(bytes.error());
        xbe_bytes = std::move(*bytes);
    } else {
        std::optional<std::vector<uint8_t>> bytes = read_whole(src);
        if (!bytes) return fail(std::string("read failed: ") + argv[1]);
        xbe_bytes = std::move(*bytes);
    }

    xbrt::Parsed<xbrt::XbeImage> parsed = xbrt::parse_xbe(xbe_bytes);
    if (!parsed) return fail(parsed.error());
    const xbrt::XbeImage &x = *parsed;
    std::printf("title: %s %s region 0x%X\n", x.cert.title_name.c_str(), x.title_id_hex().c_str(), x.cert.game_region);
    std::printf("base 0x%X image_size 0x%X entry 0x%X thunk 0x%X keys %s\n", x.base.v, x.image_size, x.entry.v,
                x.thunk_table.v, x.keys == xbrt::XbeKeys::Retail ? "retail" : x.keys == xbrt::XbeKeys::Debug ? "debug" : "chihiro");
    std::printf("sections: %zu\n", x.sections.size());
    for (const xbrt::XbeSection &s : x.sections)
        std::printf("  %-12s va 0x%08X vsize 0x%08X raw 0x%08X+0x%08X%s%s%s%s\n", s.name.c_str(), s.va.v, s.virtual_size,
                    s.raw_offset, s.raw_size, s.preload ? " preload" : "", s.executable ? " exec" : "",
                    s.writable ? " writable" : "", s.inserted_file ? " inserted" : "");
    std::printf("libraries: %zu\n", x.libraries.size());
    for (const xbrt::XbeLibrary &l : x.libraries)
        std::printf("  %-8s %u.%u.%u qfe %u%s\n", l.name.c_str(), l.major, l.minor, l.build, l.qfe, l.debug ? " debug" : "");
    std::printf("imports: %zu\n", x.imports.size());
    if (imports)
        for (xbrt::Ordinal o : x.imports)
            std::printf("  %3u %s\n", o.v, std::string(xbrt::kernel_export_name(o)).c_str());
    std::printf("tls: raw 0x%X..0x%X index 0x%X zero_fill 0x%X\n", x.tls.raw_start.v, x.tls.raw_end.v, x.tls.index_var.v,
                x.tls.zero_fill);
    std::printf("stack_commit 0x%X init_flags 0x%X\n", x.stack_commit, x.init_flags);
    std::printf("sha256 ");
    for (uint8_t b : x.sha256) std::printf("%02x", b);
    std::printf("\n");
    if (plan) print_plan(x, xbe_bytes, sites);
    return 0;
}
