// Output is metadata only (names, ordinals, versions, counts); no game bytes.
#pragma once

#include "core/plan.h"
#include "core/types.h"

#include <string>
#include <string_view>
#include <vector>

namespace xbrt {

// What the fork does for an ordinal: a bridge (Full or Partial), a stub the runtime refuses when called (Stub), a stub
// reviewed as the console's own answer (NoOp, scripts/kernel-noop-exports.tsv), a data export served from its kernel
// data page (Data), or nothing (None).
enum class BridgeStatus : uint8_t { Full, Partial, Stub, NoOp, Data, None };

BridgeStatus bridge_status(Ordinal o);

struct LibraryCoverage {
    XbeLibrary library;              // name and build from the XBE's library-version table
    bool database_has_signatures;    // XbSymbolDatabase knows this library name
    uint32_t functions_named;        // how many it found at this build
};

enum class SurveyVerdict : uint8_t {
    Ready,       // parsed, planned, no import without a bridge or with a refused stub, no unpromotable fs form
    NamedGaps,   // boots; the listed gaps stop it when reached (imports with no bridge or a stub, unpromotable fs forms)
    Unparsed,    // the disc or the XBE could not be read; reason given
};

std::string_view survey_verdict_name(SurveyVerdict v);

struct SurveyRow {
    std::string image_path;   // the disc image or XBE the row came from, as given
    std::string title_name;
    std::string title_id;     // eight hex digits, "FFFF0042"; "" when Unparsed before the certificate
    GuestVa entry;
    std::vector<std::string> xbes;   // every .xbe on the disc (root and below)
    std::vector<LibraryCoverage> libraries;
    uint32_t imports_total = 0;
    std::vector<Ordinal> imports_no_bridge, imports_stub;
    CodeMapStats code_map{};
    std::vector<HleId> overrides_placed;
    bool calls_launch_new_image = false;   // XLaunchNewImageA found by the database
    SurveyVerdict verdict = SurveyVerdict::Unparsed;
    std::string reason;   // the Unparsed reason, or a one-line summary of the gaps
};

// One row per disc image; never fails (a bad image gives an Unparsed row with the reason).
SurveyRow survey_disc(const Source &image, std::string_view path);
// One row per bare XBE file (the Mac has XBEs extracted beside the images).
SurveyRow survey_xbe(std::span<const uint8_t> file, std::string_view path);

// survey.tsv: a header line plus one line per row, stable column order (scripts/ledger.py reads it).
std::string to_tsv(const std::vector<SurveyRow> &rows);
std::string tsv_header();
std::string to_tsv_line(const SurveyRow &row);

}  // namespace xbrt
