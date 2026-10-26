#pragma once

#include "host/m0/corpus.h"

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace xbrt::m0 {

enum class Step : uint8_t { Glslang, SpirvCross, D3DCompile };
const char *step_name(Step s);

struct StepTimes {
    double glslang_ms = 0, cross_ms = 0, d3dcompile_ms = 0;
    double total() const { return glslang_ms + cross_ms + d3dcompile_ms; }
};

struct Compiled {
    std::string hlsl;
    std::vector<uint8_t> dxbc;
    std::vector<uint32_t> input_locations;   // vertex shaders: the locations SPIRV-Cross made TEXCOORD<n> inputs
    std::string rewrites;                    // the D3D rewrites applied, comma-separated; empty when none
    StepTimes ms;
};

struct ChainError {
    Step step;
    std::string message;   // one line
    StepTimes ms;          // the steps that ran, the failing one included
};

// Process-wide glslang state; call once before compile and once after the last.
void chain_init();
void chain_finalize();

std::expected<Compiled, ChainError> compile(const Shader &shader) noexcept;

}  // namespace xbrt::m0
