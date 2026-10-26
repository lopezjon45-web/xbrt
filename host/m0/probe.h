// M0 (SYNTHESIS.md): does the One S's driver accept, and can Jaguar afford, the shaders and draws the D3D11On12
// renderer will make? Over the corpus (corpus.h) the probe
//   1. clears the D3D12 implicit shader caches (probe 1.3.6 round H's ShaderCacheControl CLEAR, every kind);
//   2. runs every shader through the chain (chain.h), timing each step;
//   3. per pipeline, creates the D3D12 pipeline state directly (the driver's own HRESULT and compile time), then the
//      D3D11 objects on D3D11On12 at 11_1 and draws once under a pipeline-statistics query: a draw the layer dropped
//      shows zero vertex-shader invocations;
//   4. draws 1,000 small triangles a frame with a pipeline change every other draw, and times CPU per draw and frame.
// Every result is a row of <out_dir>\m0.tsv, flushed before the next step. A "begin" row precedes each shader and
// pipeline, so a launch that finds an unfinished begin records that item as "died" and carries on after it: a driver
// that kills the process costs one relaunch, not the visit. A structured exception or C++ throw inside one pipeline is
// that pipeline's "thrown" row, and the device is recreated for the next.
#pragma once

#include <functional>
#include <string>

namespace xbrt::m0 {

struct ProbeOptions {
    std::string corpus;       // the bundle's bytes
    std::wstring out_dir;     // m0.tsv is written (or resumed) here
    bool warp = false;        // the WARP adapter (CI) instead of the default one (the console)
    unsigned bench_frames = 200;
};

// Status lines for run.txt and the screen: "m0 ..." on progress, "m0 FAIL ..." when the probe cannot run at all.
using LineSink = std::function<void(const std::string &)>;

enum class Outcome {
    Done,       // m0.tsv has its end row, whatever the pipelines did
    Relaunch,   // a pipeline broke the device or threw a structured exception: start the process again to go on
    Failed,     // the probe cannot run (no corpus, no device, no m0.tsv)
};

Outcome run_probe(const ProbeOptions &options, const LineSink &sink);

}  // namespace xbrt::m0
