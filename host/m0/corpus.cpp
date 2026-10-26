#include "host/m0/corpus.h"

#include <charconv>
#include <map>

namespace xbrt::m0 {

namespace {

struct Cursor {
    std::string_view rest;
    std::string_view line()
    {
        size_t n = rest.find('\n');
        std::string_view l = rest.substr(0, n);
        rest = n == std::string_view::npos ? std::string_view{} : rest.substr(n + 1);
        return l;
    }
};

std::vector<std::string_view> split(std::string_view s, char sep)
{
    std::vector<std::string_view> out;
    for (size_t at = 0;;) {
        size_t n = s.find(sep, at);
        out.push_back(s.substr(at, n - at));
        if (n == std::string_view::npos) return out;
        at = n + 1;
    }
}

template <class T>
bool number(std::string_view s, T &out)
{
    auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && end == s.data() + s.size();
}

template <class... T>
bool numbers(std::string_view s, T &...out)
{
    std::vector<std::string_view> f = split(s, ':');
    if (f.size() != sizeof...(T)) return false;
    size_t i = 0;
    return (number(f[i++], out) && ...);
}

bool stencil(std::string_view s, StencilSide &o)
{
    return numbers(s, o.fail, o.pass, o.depth_fail, o.compare, o.compare_mask, o.write_mask, o.reference);
}

}  // namespace

Parsed<Corpus> parse_corpus(std::string_view bundle)
{
    Cursor c{bundle};
    if (c.line() != "xbrt-m0-corpus 1") return std::unexpected("corpus: not an xbrt-m0-corpus 1 bundle");
    Corpus corpus;
    std::map<std::string, int, std::less<>> by_hash;
    int line_no = 1;
    while (!c.rest.empty()) {
        std::string_view l = c.line();
        line_no++;
        std::string where = "corpus line " + std::to_string(line_no) + ": ";
        std::vector<std::string_view> f = split(l, ' ');
        if (f[0] == "shader" && f.size() == 4) {
            Shader s;
            s.glsl_sha256 = f[1];
            if (f[2] == "v") s.stage = Stage::Vertex;
            else if (f[2] == "g") s.stage = Stage::Geometry;
            else if (f[2] == "f") s.stage = Stage::Pixel;
            else return std::unexpected(where + "unknown stage " + std::string(f[2]));
            size_t bytes = 0;
            if (!number(f[3], bytes) || bytes + 1 > c.rest.size()) return std::unexpected(where + "bad length");
            s.glsl = c.rest.substr(0, bytes);
            c.rest = c.rest.substr(bytes + 1);
            by_hash.emplace(s.glsl_sha256, int(corpus.shaders.size()));
            corpus.shaders.push_back(std::move(s));
            continue;
        }
        if (f[0] != "pipeline" || f.size() < 4) return std::unexpected(where + "expected shader or pipeline");
        Pipeline p;
        auto index = [&](std::string_view h, int &out) {
            if (h == "-") return true;
            auto it = by_hash.find(h);
            if (it == by_hash.end()) return false;
            out = it->second;
            return true;
        };
        if (!index(f[1], p.vs) || !index(f[2], p.gs) || !index(f[3], p.ps) || p.vs < 0 || p.ps < 0)
            return std::unexpected(where + "names a shader the bundle does not hold");
        for (size_t i = 4; i < f.size(); i++) {
            size_t eq = f[i].find('=');
            if (eq == std::string_view::npos) return std::unexpected(where + "token without '='");
            std::string_view k = f[i].substr(0, eq), v = f[i].substr(eq + 1);
            bool ok = true;
            if (k == "topology") ok = number(v, p.topology);
            else if (k == "polygon") ok = number(v, p.polygon);
            else if (k == "cull") ok = number(v, p.cull);
            else if (k == "front") ok = number(v, p.front);
            else if (k == "clamp") { int b = 0; ok = number(v, b); p.clamp = b; }
            else if (k == "color") ok = number(v, p.color_format);
            else if (k == "zeta") ok = number(v, p.zeta_format);
            else if (k == "push") ok = number(v, p.push_bytes);
            else if (k == "dtest") { int b = 0; ok = number(v, b); p.depth.emplace().test = b; }
            else if (k == "dwrite") { int b = 0; ok = p.depth && number(v, b); if (ok) p.depth->write = b; }
            else if (k == "dcmp") ok = p.depth && number(v, p.depth->compare);
            else if (k == "stencil") { int b = 0; ok = p.depth && number(v, b); if (ok) p.depth->stencil = b; }
            else if (k == "sf") ok = p.depth && stencil(v, p.depth->front);
            else if (k == "sb") ok = p.depth && stencil(v, p.depth->back);
            else if (k == "blend") {
                BlendState &b = p.blend.emplace();
                int en = 0;
                ok = numbers(v, en, b.src_color, b.dst_color, b.color_op, b.src_alpha, b.dst_alpha, b.alpha_op);
                b.enable = en;
            }
            else if (k == "wmask") ok = p.blend && number(v, p.blend->write_mask);
            else if (k == "bconst") {
                std::vector<std::string_view> c4 = split(v, ':');
                ok = p.blend && c4.size() == 4;
                for (size_t j = 0; ok && j < 4; j++) ok = number(c4[j], p.blend->constants[j]);
            }
            else if (k == "bind") {
                VertexBinding &b = p.bindings.emplace_back();
                ok = numbers(v, b.binding, b.stride, b.rate);
            }
            else if (k == "attr") {
                VertexAttribute &a = p.attributes.emplace_back();
                ok = numbers(v, a.location, a.binding, a.format, a.offset);
            }
            else if (k == "suite") p.suite = v;
            else return std::unexpected(where + "unknown key " + std::string(k));
            if (!ok) return std::unexpected(where + "bad value for " + std::string(k));
        }
        corpus.pipelines.push_back(std::move(p));
    }
    if (corpus.pipelines.empty()) return std::unexpected("corpus: no pipelines");
    return corpus;
}

}  // namespace xbrt::m0
