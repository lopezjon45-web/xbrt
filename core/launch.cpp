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
    std::optional<std::u16string> disc, survey, back, xbe, fps, trace;
    for (const auto &[name, value] : pairs) {
        if (name == u"disc") disc = value;
        else if (name == u"survey") survey = value;
        else if (name == u"launchOnExit") back = value;
        else if (name == u"xbe") xbe = value;
        else if (name == u"fps") fps = value;
        else if (name == u"trace") trace = value;
    }

    LaunchRequest r;
    if (!back || back->empty()) return std::unexpected("launchOnExit: missing, so there is no way back to the hub");
    r.return_uri = std::move(*back);

    if (disc && survey) return std::unexpected("disc and survey: a link names one or the other");
    if (!disc && !survey) return std::unexpected("disc: missing, and no survey either");
    if (disc) {
        if (!drive_absolute(*disc)) return bad("disc", *disc, "is not an absolute path on a drive");
        if (!ends_with_ci(*disc, u".iso") && !ends_with_ci(*disc, u".xiso"))
            return bad("disc", *disc, "is not an .iso or .xiso");
        r.disc_path = std::move(*disc);
    } else {
        if (!drive_absolute(*survey)) return bad("survey", *survey, "is not an absolute path on a drive");
        r.survey_folder = std::move(*survey);
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

std::u16string return_link(std::u16string_view return_uri, std::string_view result, std::string_view kind,
                           std::string_view missing)
{
    std::u16string out(return_uri);
    if (!out.ends_with(u'?') && !out.ends_with(u'&')) out += out.find(u'?') == std::u16string::npos ? u'?' : u'&';
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
