#include "host/m0/chain.h"

#include <windows.h>

#include <d3dcompiler.h>
#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>
#include <spirv_hlsl.hpp>

#include <memory>

namespace xbrt::m0 {

namespace {

// xemu binds its UBOs and textures by Vulkan binding number, which SPIRV-Cross keeps as the register index; the push
// constant block has no binding, so it gets a CBV slot nothing else uses.
constexpr uint32_t kPushConstantRegister = 12;

double now_ms()
{
    static const double ticks_per_ms = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return double(f.QuadPart) / 1000.0;
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart) / ticks_per_ms;
}

std::string one_line(std::string s)
{
    for (char &c : s)
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    if (s.size() > 300) s.resize(300);
    return s;
}

glslang_stage_t glslang_stage(Stage s)
{
    switch (s) {
    case Stage::Vertex: return GLSLANG_STAGE_VERTEX;
    case Stage::Geometry: return GLSLANG_STAGE_GEOMETRY;
    case Stage::Pixel: return GLSLANG_STAGE_FRAGMENT;
    }
    return GLSLANG_STAGE_VERTEX;
}

const char *target(Stage s)
{
    switch (s) {
    case Stage::Vertex: return "vs_5_0";
    case Stage::Geometry: return "gs_5_0";
    case Stage::Pixel: return "ps_5_0";
    }
    return "vs_5_0";
}

using ShaderPtr = std::unique_ptr<glslang_shader_t, decltype(&glslang_shader_delete)>;
using ProgramPtr = std::unique_ptr<glslang_program_t, decltype(&glslang_program_delete)>;

// D3D has no point size, and SPIRV-Cross refuses a geometry shader that reads gl_in[].gl_PointSize (SPIR-V BuiltIn 1,
// "Unsupported builtin in HLSL"); xemu's geometry shaders copy it through on every vertex.
constexpr std::string_view kGsPointSize = "  gl_PointSize = gl_in[index].gl_PointSize;\n";

std::string rewrite_for_d3d(const Shader &shader, std::string &rewrites)
{
    std::string glsl = shader.glsl;
    if (shader.stage == Stage::Geometry) {
        bool changed = false;
        for (size_t at; (at = glsl.find(kGsPointSize)) != std::string::npos; changed = true) glsl.erase(at, kGsPointSize.size());
        if (changed) rewrites = "gs-point-size";
    }
    return glsl;
}

std::expected<std::vector<uint32_t>, std::string> to_spirv(const std::string &glsl, Stage stage)
{
    glslang_input_t input{};
    input.language = GLSLANG_SOURCE_GLSL;
    input.stage = glslang_stage(stage);
    input.client = GLSLANG_CLIENT_VULKAN;
    input.client_version = GLSLANG_TARGET_VULKAN_1_1;
    input.target_language = GLSLANG_TARGET_SPV;
    input.target_language_version = GLSLANG_TARGET_SPV_1_3;
    input.code = glsl.c_str();
    input.default_version = 460;
    input.default_profile = GLSLANG_NO_PROFILE;
    input.messages = GLSLANG_MSG_DEFAULT_BIT;
    input.resource = glslang_default_resource();

    ShaderPtr sh(glslang_shader_create(&input), glslang_shader_delete);
    if (!glslang_shader_preprocess(sh.get(), &input))
        return std::unexpected("preprocess: " + one_line(glslang_shader_get_info_log(sh.get())));
    if (!glslang_shader_parse(sh.get(), &input))
        return std::unexpected("parse: " + one_line(glslang_shader_get_info_log(sh.get())));
    ProgramPtr program(glslang_program_create(), glslang_program_delete);
    glslang_program_add_shader(program.get(), sh.get());
    if (!glslang_program_link(program.get(), GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT))
        return std::unexpected("link: " + one_line(glslang_program_get_info_log(program.get())));
    glslang_spv_options_t options{};
    options.disable_optimizer = true;
    glslang_program_SPIRV_generate_with_options(program.get(), input.stage, &options);
    std::vector<uint32_t> words(glslang_program_SPIRV_get_size(program.get()));
    glslang_program_SPIRV_get(program.get(), words.data());
    if (words.empty()) return std::unexpected("no SPIR-V");
    return words;
}

std::string to_hlsl(std::vector<uint32_t> spirv, Stage stage, std::vector<uint32_t> &input_locations)
{
    spirv_cross::CompilerHLSL hlsl(std::move(spirv));
    spirv_cross::CompilerHLSL::Options options;
    options.shader_model = 50;
    options.point_size_compat = true;
    options.point_coord_compat = true;
    hlsl.set_hlsl_options(options);
    spirv_cross::HLSLResourceBinding push;
    push.stage = hlsl.get_execution_model();
    push.desc_set = spirv_cross::ResourceBindingPushConstantDescriptorSet;
    push.binding = spirv_cross::ResourceBindingPushConstantBinding;
    push.cbv.register_binding = kPushConstantRegister;
    hlsl.add_hlsl_resource_binding(push);
    if (stage == Stage::Vertex) {
        for (const spirv_cross::Resource &r : hlsl.get_shader_resources().stage_inputs)
            if (hlsl.has_decoration(r.id, spv::DecorationLocation))
                input_locations.push_back(hlsl.get_decoration(r.id, spv::DecorationLocation));
    }
    return hlsl.compile();
}

}  // namespace

const char *step_name(Step s)
{
    switch (s) {
    case Step::Glslang: return "glslang";
    case Step::SpirvCross: return "spirv-cross";
    case Step::D3DCompile: return "d3dcompile";
    }
    return "?";
}

void chain_init() { glslang_initialize_process(); }
void chain_finalize() { glslang_finalize_process(); }

std::expected<Compiled, ChainError> compile(const Shader &shader) noexcept
{
    Compiled out;
    double t0 = now_ms();
    std::expected<std::vector<uint32_t>, std::string> spirv;
    try {
        spirv = to_spirv(rewrite_for_d3d(shader, out.rewrites), shader.stage);
    } catch (const std::exception &e) {
        spirv = std::unexpected(std::string("threw: ") + e.what());
    }
    double t1 = now_ms();
    out.ms.glslang_ms = t1 - t0;
    if (!spirv) return std::unexpected(ChainError{Step::Glslang, spirv.error(), out.ms});

    try {
        out.hlsl = to_hlsl(std::move(*spirv), shader.stage, out.input_locations);
    } catch (const std::exception &e) {
        out.ms.cross_ms = now_ms() - t1;
        return std::unexpected(ChainError{Step::SpirvCross, one_line(e.what()), out.ms});
    }
    double t2 = now_ms();
    out.ms.cross_ms = t2 - t1;

    ID3DBlob *code = nullptr, *errors = nullptr;
    HRESULT hr = D3DCompile(out.hlsl.data(), out.hlsl.size(), nullptr, nullptr, nullptr, "main", target(shader.stage),
                            D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &errors);
    out.ms.d3dcompile_ms = now_ms() - t2;
    std::string log = errors ? one_line(std::string(static_cast<const char *>(errors->GetBufferPointer()),
                                                    errors->GetBufferSize()))
                             : "";
    if (errors) errors->Release();
    if (FAILED(hr) || !code) {
        if (code) code->Release();
        char buf[32];
        snprintf(buf, sizeof buf, "hr=0x%08lx ", static_cast<unsigned long>(hr));
        return std::unexpected(ChainError{Step::D3DCompile, buf + log, out.ms});
    }
    const uint8_t *p = static_cast<const uint8_t *>(code->GetBufferPointer());
    out.dxbc.assign(p, p + code->GetBufferSize());
    code->Release();
    return out;
}

}  // namespace xbrt::m0
