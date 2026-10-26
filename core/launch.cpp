#include "core/launch.h"

#include <optional>

namespace xbrt {

namespace {

char16_t lower(char16_t c) { return c >= u'A' && c <= u'Z' ? char16_t(c - u'A' + u'a') : c; }

bool ends_with_ci(std::u16string_view s, std::u16string_view suffix)
{
    if (s.size() < suffix.size()) return false;
    for (size_t i = 0; i < suffix.size(); i++)
        if (lower(s[s.size() - suffix.size() + i]) != suffix[i]) return false;
    return true;
}

bool drive_absolute(std::u16string_view p)
{
    return p.size() >= 3 && lower(p[0]) >= u'a' && lower(p[0]) <= u'z' && p[1] == u':' && p[2] == u'\\';
}

std::string ascii(std::u16string_view s)
{
    std::string out;
    for (char16_t c : s) out += c >= 0x20 && c < 0x7F ? char(c) : '?';
    return out;
}

std::unexpected<std::string> bad(const char *field, std::u16string_view value, const char *why)
{
    return std::unexpected(std::string(field) + ": \"" + ascii(value) + "\" " + why);
}

void append_escaped(std::u16string &out, std::string_view utf8)
{
    static const char hex[] = "0123456789ABCDEF";
    for (unsigned char c : utf8) {
        bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                          c == '.' || c == '_' || c == '~';
        if (unreserved) {
            out += char16_t(c);
        } else {
            out += u'%';
            out += char16_t(hex[c >> 4]);
            out += char16_t(hex[c & 0xF]);
        }
    }
}

}  // namespace

Parsed<LaunchRequest> parse_launch(const QueryPairs &pairs)
{
    std::optional<std::u16string> disc, dir, survey, back, xbe, fps, trace, home;
    for (const auto &[name, value] : pairs) {
        if (name == u"disc") disc = value;
        else if (name == u"dir") dir = value;
        else if (name == u"home") home = value;
        else if (name == u"survey") survey = value;
        else if (name == u"launchOnExit") back = value;
        else if (name == u"xbe") xbe = value;
        else if (name == u"fps") fps = value;
        else if (name == u"trace") trace = value;
    }

    LaunchRequest r;
    if (back) r.return_uri = std::move(*back);

    if (int(disc.has_value()) + int(dir.has_value()) + int(survey.has_value()) != 1)
        return std::unexpected("disc, dir or survey: a link names exactly one of them");
    if (disc) {
        if (!drive_absolute(*disc)) return bad("disc", *disc, "is not an absolute path on a drive");
        if (!ends_with_ci(*disc, u".iso") && !ends_with_ci(*disc, u".xiso"))
            return bad("disc", *disc, "is not an .iso or .xiso");
        r.disc_path = std::move(*disc);
    } else if (dir) {
        if (dir->empty() || dir->find(u"..") != std::u16string::npos)
            return bad("dir", *dir, "is empty or climbs out of its folder");
        r.dir_path = std::move(*dir);
    } else {
        if (!drive_absolute(*survey)) return bad("survey", *survey, "is not an absolute path on a drive");
        r.survey_folder = std::move(*survey);
    }
    if (home) {
        if (!drive_absolute(*home)) return bad("home", *home, "is not an absolute path on a drive");
        r.home = std::move(*home);
    }

    if (xbe) {
        if (xbe->size() <= 5 || (*xbe)[0] != u'\\' || !ends_with_ci(*xbe, u".xbe"))
            return bad("xbe", *xbe, "is not a disc path like \\default.xbe");
        r.xbe = std::move(*xbe);
    }
    if (fps) {
        if (*fps != u"0" && *fps != u"1") return bad("fps", *fps, "is not 0 or 1");
        r.show_counters = *fps == u"1";
    }
    if (trace) {
        if (*trace == u"off") r.trace = Trace::Off;
        else if (*trace == u"kernel") r.trace = Trace::Kernel;
        else if (*trace == u"traps") r.trace = Trace::Traps;
        else return bad("trace", *trace, "is not off, kernel or traps");
    }
    return r;
}

// INTEGRATION.md section 1's kinds besides Launch: the refusals a running title meets.
constexpr std::string_view kRefusalKinds[] = {"KernelExport", "Instruction", "DeviceRegister", "GpuMethod", "Feature",
                                              "Hang"};

ReturnFailure return_failure(std::string_view kind, std::string_view text)
{
    for (std::string_view k : kRefusalKinds)
        if (k == kind) return {std::string(kind), std::string(text)};
    std::string fail = std::string(kind) + " FAIL ";
    if (text.starts_with(fail)) text.remove_prefix(fail.size());
    return {"Launch", std::string(kind) + ": " + std::string(text)};
}

std::u16string return_link(std::u16string_view return_uri, std::string_view result, std::string_view kind,
                           std::string_view missing)
{
    std::u16string out(return_uri);
    auto separate = [&out] {
        if (!out.ends_with(u'?') && !out.ends_with(u'&')) out += out.find(u'?') == std::u16string::npos ? u'?' : u'&';
    };
    // A from= the link already carries is replaced: the hub reads from=xbrt to know who returned.
    size_t query = out.find(u'?');
    size_t from = std::u16string::npos;
    if (query != std::u16string::npos) {
        for (size_t at = query; at != std::u16string::npos && from == std::u16string::npos; at = out.find(u'&', at + 1))
            if (std::u16string_view(out).substr(at + 1).starts_with(u"from=")) from = at + 1;
    }
    if (from != std::u16string::npos) {
        size_t end = out.find(u'&', from);
        out.replace(from, (end == std::u16string::npos ? out.size() : end) - from, u"from=xbrt");
    } else {
        separate();
        out += u"from=xbrt";
    }
    separate();
    out += u"result=";
    append_escaped(out, result);
    if (result != "ok") {
        out += u"&kind=";
        append_escaped(out, kind);
        out += u"&missing=";
        append_escaped(out, missing);
    }
    return out;
}

}  // namespace xbrt
