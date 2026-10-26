//   xboxrt:?disc=<esc(E:\Xbox\<name>.iso)>&home=<esc(E:\Game Vault)>&launchOnExit=<esc(return link)>
//          [&xbe=<esc(\x.xbe)>][&fps=1][&trace=kernel]
//   xboxrt:?dir=<esc(disc)>&launchOnExit=...        an extracted disc: absolute, or a folder under LocalState
//   xboxrt:?survey=<esc(E:\Xbox)>&launchOnExit=<esc(return link)>
// The shape mirrors Game Vault's xemu link (INTEGRATION.md section 1); `home` is where saves and per-game data live.
#pragma once

#include "core/types.h"

#include <string>
#include <utility>
#include <vector>

namespace xbrt {

enum class Trace : uint8_t { Off, Kernel, Traps };

struct LaunchRequest {
    std::u16string disc_path;       // absolute, a drive letter, ends in .iso or .xiso; empty for dir or survey
    std::u16string dir_path;        // an extracted disc directory (absolute, or relative to LocalState); D:\ maps to it
    std::u16string xbe = u"\\default.xbe";
    std::u16string home;            // <home>\saves\xbrt\<TitleID>\ holds saves; empty means LocalState
    std::u16string return_uri;      // there is always a way back to the hub
    std::u16string survey_folder;   // xboxrt:?survey=<folder>: no game; survey every *.iso there
    bool show_counters = false;
    Trace trace = Trace::Off;
};

using QueryPairs = std::vector<std::pair<std::u16string, std::u16string>>;

// Unknown names are ignored. Missing launchOnExit, neither disc nor survey, a disc path that is not absolute or not
// .iso/.xiso, or an xbe that is not "\...\.xbe" is an error naming the field.
Parsed<LaunchRequest> parse_launch(const QueryPairs &pairs);

struct ReturnFailure {
    std::string kind, missing;
};

// What the return link names for a failure the shell recorded under `kind` with the screen line `text`.
ReturnFailure return_failure(std::string_view kind, std::string_view text);

// The return link with the outcome appended, as the hub reads it: "<return_uri>[?&]from=xbrt&result=ok" or
// "...result=error&kind=<kind>&missing=<escaped text>". `from=` is added only when the uri carries none.
std::u16string return_link(std::u16string_view return_uri, std::string_view result, std::string_view kind,
                           std::string_view missing);

}  // namespace xbrt
