#pragma once

#include <functional>
#include <string>

namespace xbrt::m0 {

struct ProbeOptions {
    std::string corpus;
    std::wstring out_dir;
    bool warp = false;
    unsigned bench_frames = 200;
};

using LineSink = std::function<void(const std::string &)>;

enum class Outcome {
    Done,
    Relaunch,
    Failed,
};

Outcome run_probe(const ProbeOptions &options, const LineSink &sink);

}  // namespace xbrt::m0
