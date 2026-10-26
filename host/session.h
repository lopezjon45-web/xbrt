#pragma once

#include "core/disc.h"
#include "core/launch.h"
#include "core/types.h"
#include "core/xbe.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace xbrt {

enum class BootStage : uint8_t { Reserve, Link, OpenDisc, ReadXbe, Analyze, MapImage, Kernel, Devices, Start };

std::string_view boot_stage_name(BootStage s);

struct BootError {
    BootStage stage;
    std::string detail;   // one line for the screen, the full text in run.txt
};

struct ShellServices {
    virtual void stage(BootStage s, std::string_view line) = 0;   // green line on the status screen
    virtual void fail(BootStage s, std::string_view line) = 0;    // red line; the screen stays until B
    virtual std::unique_ptr<Source> open_disc(std::u16string_view path, std::string &error) = 0;
    // Every *.iso and *.xiso in `folder`, full paths, sorted.
    virtual std::vector<std::u16string> list_discs(std::u16string_view folder, std::string &error) = 0;
    virtual std::u16string data_root() = 0;   // LocalState

protected:
    ~ShellServices() = default;
};

class Session {
public:
    // Stages OpenDisc and ReadXbe, each reported to `shell` as it passes and on failure. The disc stays open for
    // the session's life (D:\ is read in place).
    static std::expected<Session, BootError> boot(const LaunchRequest &request, ShellServices &shell);

    const XbeImage &xbe() const { return xbe_; }
    const DiscIndex &disc() const { return disc_; }

private:
    std::unique_ptr<Source> image_;
    DiscIndex disc_;
    XbeImage xbe_;
};

// One line for the ReadXbe stage: "SYNTH FFFF0042 XDK 5849, 3 imports".
std::string xbe_summary_line(const XbeImage &xbe);

struct SurveyTotals {
    uint32_t images, parsed, ready, named_gaps, unparsed, distinct_builds;
    std::vector<std::pair<Ordinal, uint32_t>> no_bridge_by_titles;   // ordinal, how many titles import it; sorted
};

// xboxrt:?survey=<folder>: survey_disc over every image the shell lists, one progress line per image through
// `progress` ("survey 3/12 <file>: <verdict>"), survey.tsv rewritten atomically under data_root() after each image.
// No guest code runs. Returns the totals, or the reason the folder could not be listed.
Parsed<SurveyTotals> run_survey(std::u16string_view folder, ShellServices &shell,
                                const std::function<void(std::string_view)> &progress);

}  // namespace xbrt
