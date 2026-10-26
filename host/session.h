#pragma once

#include "core/disc.h"
#include "core/launch.h"
#include "core/types.h"
#include "core/xbe.h"
#include "host/cpu.h"
#include "host/kernel.h"
#include "host/memory.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
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
    virtual void log(std::string_view line) = 0;                  // the kernel's and the guest's lines: run.txt only
    virtual void counters(std::string_view line) = 0;             // once a second while the link asks for fps=1
    virtual std::unique_ptr<Source> open_disc(std::u16string_view path, std::string &error) = 0;
    // Every *.iso and *.xiso in `folder`, full paths, sorted.
    virtual std::vector<std::u16string> list_discs(std::u16string_view folder, std::string &error) = 0;
    virtual std::u16string data_root() = 0;   // LocalState
    // The Devices stage: a host with a device to register (the desktop's loopback port) maps it here.
    virtual void attach_devices(Cpu &) {}

protected:
    ~ShellServices() = default;
};

// How Session::run ended. ReturnedToHub is result=ok for the hub; GuestExited (every guest thread ended without
// returning to the hub) is an error the shell names; Fatal carries a Missing.
enum class ExitReason : uint8_t { ReturnedToHub, GuestExited, Fatal };

std::string_view exit_reason_name(ExitReason r);

// The red line and crash.txt's first line: "<Kind>: <what> at <where_symbol>".
std::string missing_line(const Missing &m);

class Session {
public:
    // Stages OpenDisc through Start, each reported to `shell` as it passes and on failure. Start spawns guest thread
    // 0 at the XBE entry; run() must follow. The disc stays open for the session's life (D:\ is read in place).
    static std::expected<Session, BootError> boot(const LaunchRequest &request, GuestMemory &memory, ShellServices &shell);

    // Blocks until the guest returns to the hub, every guest thread has ended, a stop-kind Missing arrives, or the
    // watchdog sees no gate call for 10 s. Every stop writes <data root>\crash.txt. The sites learned from traps
    // are merged into the plan and written to <data root>\titles\<sha256>\plan.bin on every exit.
    ExitReason run();

    const XbeImage &xbe() const;
    const DiscIndex &disc() const;
    const Missing *stop() const;   // after a Fatal run()

    Session(Session &&) noexcept;
    Session &operator=(Session &&) noexcept;
    ~Session();

    struct State;

private:
    Session();
    std::unique_ptr<State> s_;
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
