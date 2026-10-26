// The M0 pipeline corpus: every pipeline upstream xemu's Vulkan renderer created while running nxdk_pgraph_tests, with
// the GLSL its generators emitted and the Vulkan fixed-function state it was created with (tools/m0/pack-corpus.py
// writes the bundle). Enum fields hold the Vulkan values; probe.cpp maps them to D3D.
#pragma once

#include "core/types.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xbrt::m0 {

enum class Stage : uint8_t { Vertex, Geometry, Pixel };

struct Shader {
    std::string hash;   // sha256 of the GLSL
    Stage stage;
    std::string glsl;
};

struct StencilSide {
    int fail = 0, pass = 0, depth_fail = 0, compare = 0;
    uint32_t compare_mask = 0, write_mask = 0, reference = 0;
};

struct DepthState {
    bool test = false, write = false, stencil = false;
    int compare = 0;
    StencilSide front, back;
};

struct BlendState {
    bool enable = false;
    int src_color = 0, dst_color = 0, color_op = 0, src_alpha = 0, dst_alpha = 0, alpha_op = 0;
    uint32_t write_mask = 0xF;
    float constants[4] = {};
};

struct VertexBinding {
    uint32_t binding = 0, stride = 0, rate = 0;
};

struct VertexAttribute {
    uint32_t location = 0, binding = 0, offset = 0;
    int format = 0;
};

struct Pipeline {
    int vs = -1, gs = -1, ps = -1;   // indices into Corpus::shaders; gs is -1 when there is none
    int topology = 0, polygon = 0, front = 0;
    uint32_t cull = 0;
    bool clamp = false;
    int color_format = 0, zeta_format = 0;
    uint32_t push_bytes = 0;
    std::optional<DepthState> depth;   // absent when the pipeline had no depth attachment
    std::optional<BlendState> blend;   // absent when it had no color attachment
    std::vector<VertexBinding> bindings;
    std::vector<VertexAttribute> attributes;
};

struct Corpus {
    std::vector<Shader> shaders;
    std::vector<Pipeline> pipelines;
};

Parsed<Corpus> parse_corpus(std::string_view bundle);

}  // namespace xbrt::m0
