#include "host/m0/probe.h"

#include "host/m0/chain.h"
#include "host/m0/corpus.h"

#include <windows.h>

#include <d3d11_1.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>

using Microsoft::WRL::ComPtr;

namespace xbrt::m0 {

namespace {

struct Tsv {
    FILE *f = nullptr;
    template <class... T>
    void row(const T &...cols)
    {
        std::ostringstream s;
        size_t i = 0;
        ((s << (i++ ? "\t" : "") << cols), ...);
        s << '\n';
        std::string line = s.str();
        fwrite(line.data(), 1, line.size(), f);
        fflush(f);
    }
};

struct Resume {
    std::set<long> shaders, pipelines;
    bool bench = false, ended = false;
    int launches = 0;
    std::optional<std::pair<std::string, long>> unfinished_begin;
};

std::vector<std::string> fields(const std::string &line)
{
    std::vector<std::string> out;
    std::stringstream s(line);
    std::string f;
    while (std::getline(s, f, '\t')) out.push_back(f);
    return out;
}

Resume read_resume(const std::wstring &path)
{
    Resume r;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> f = fields(line);
        if (f.empty()) continue;
        if (f[0] == "begin" && f.size() >= 3) r.unfinished_begin = std::make_pair(f[1], std::stol(f[2]));
        else if (f[0] == "shader" && f.size() >= 2) { r.shaders.insert(std::stol(f[1])); r.unfinished_begin.reset(); }
        else if (f[0] == "pipeline" && f.size() >= 2) { r.pipelines.insert(std::stol(f[1])); r.unfinished_begin.reset(); }
        else if (f[0] == "benchdone") { r.bench = true; r.unfinished_begin.reset(); }
        else if (f[0] == "launch") r.launches++;
        else if (f[0] == "end") r.ended = true;
    }
    return r;
}

std::string hex(uint32_t v)
{
    char b[16];
    snprintf(b, sizeof b, "0x%08X", v);
    return b;
}

std::string fixed(double v, int digits = 3)
{
    char b[32];
    snprintf(b, sizeof b, "%.*f", digits, v);
    return b;
}

double now_ms()
{
    static const double per_ms = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return double(f.QuadPart) / 1000.0;
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart) / per_ms;
}

double thread_cpu_ms()
{
    FILETIME c, e, k, u;
    GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u);
    auto v = [](FILETIME t) { return (uint64_t(t.dwHighDateTime) << 32 | t.dwLowDateTime) / 10000.0; };
    return v(k) + v(u);
}

double process_cpu_ms()
{
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    auto v = [](FILETIME t) { return (uint64_t(t.dwHighDateTime) << 32 | t.dwLowDateTime) / 10000.0; };
    return v(k) + v(u);
}

double percentile(std::vector<double> v, double p)
{
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, size_t(p * (v.size() - 1) + 0.5))];
}

struct VertexBuffer {
    UINT slot, stride;
};

struct Mapped {
    D3D11_BLEND_DESC blend{};
    D3D11_RASTERIZER_DESC raster{};
    D3D11_DEPTH_STENCIL_DESC depth{};
    std::vector<D3D11_INPUT_ELEMENT_DESC> layout;
    std::vector<VertexBuffer> buffers;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    D3D12_PRIMITIVE_TOPOLOGY_TYPE type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    UINT vertices = 3;
    DXGI_FORMAT rt = DXGI_FORMAT_UNKNOWN, ds = DXGI_FORMAT_UNKNOWN;
    std::vector<std::string> notes;
};

constexpr UINT kSpareSlot = D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT - 1;

DXGI_FORMAT vertex_format(int vk, std::vector<std::string> &notes)
{
    switch (vk) {
    case 100: return DXGI_FORMAT_R32_FLOAT;
    case 103: return DXGI_FORMAT_R32G32_FLOAT;
    case 106: return DXGI_FORMAT_R32G32B32_FLOAT;
    case 109: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case 9: return DXGI_FORMAT_R8_UNORM;
    case 16: return DXGI_FORMAT_R8G8_UNORM;
    case 23: notes.push_back("vertex-rgb8-widened"); return DXGI_FORMAT_R8G8B8A8_UNORM;
    case 37: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case 71: return DXGI_FORMAT_R16_SNORM;
    case 78: return DXGI_FORMAT_R16G16_SNORM;
    case 85: notes.push_back("vertex-rgb16-widened"); return DXGI_FORMAT_R16G16B16A16_SNORM;
    case 92: return DXGI_FORMAT_R16G16B16A16_SNORM;
    case 73: notes.push_back("vertex-sscaled"); return DXGI_FORMAT_R16_SINT;
    case 80: notes.push_back("vertex-sscaled"); return DXGI_FORMAT_R16G16_SINT;
    case 87: notes.push_back("vertex-sscaled"); return DXGI_FORMAT_R16G16B16A16_SINT;
    case 94: notes.push_back("vertex-sscaled"); return DXGI_FORMAT_R16G16B16A16_SINT;
    case 99: return DXGI_FORMAT_R32_SINT;
    default: notes.push_back("vertex-format-" + std::to_string(vk)); return DXGI_FORMAT_R32G32B32A32_FLOAT;
    }
}

DXGI_FORMAT color_format(int vk, std::vector<std::string> &notes)
{
    switch (vk) {
    case 0: return DXGI_FORMAT_UNKNOWN;
    case 4: return DXGI_FORMAT_B5G6R5_UNORM;
    case 8: return DXGI_FORMAT_B5G5R5A1_UNORM;
    case 9: return DXGI_FORMAT_R8_UNORM;
    case 16: return DXGI_FORMAT_R8G8_UNORM;
    case 44: return DXGI_FORMAT_B8G8R8A8_UNORM;
    default: notes.push_back("color-format-" + std::to_string(vk)); return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

DXGI_FORMAT depth_format(int vk, std::vector<std::string> &notes)
{
    switch (vk) {
    case 0: return DXGI_FORMAT_UNKNOWN;
    case 124: return DXGI_FORMAT_D16_UNORM;
    case 129: return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case 130: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    default: notes.push_back("depth-format-" + std::to_string(vk)); return DXGI_FORMAT_D24_UNORM_S8_UINT;
    }
}

D3D11_BLEND blend_factor(int vk, bool alpha, std::vector<std::string> &notes)
{
    static const D3D11_BLEND map[] = {
        D3D11_BLEND_ZERO,         D3D11_BLEND_ONE,          D3D11_BLEND_SRC_COLOR,      D3D11_BLEND_INV_SRC_COLOR,
        D3D11_BLEND_DEST_COLOR,   D3D11_BLEND_INV_DEST_COLOR, D3D11_BLEND_SRC_ALPHA,    D3D11_BLEND_INV_SRC_ALPHA,
        D3D11_BLEND_DEST_ALPHA,   D3D11_BLEND_INV_DEST_ALPHA, D3D11_BLEND_BLEND_FACTOR, D3D11_BLEND_INV_BLEND_FACTOR,
        D3D11_BLEND_BLEND_FACTOR, D3D11_BLEND_INV_BLEND_FACTOR, D3D11_BLEND_SRC_ALPHA_SAT,
    };
    if (vk < 0 || vk >= int(std::size(map))) {
        notes.push_back("blend-factor-" + std::to_string(vk));
        return D3D11_BLEND_ONE;
    }
    if (vk == 12 || vk == 13) notes.push_back("blend-constant-alpha");
    D3D11_BLEND b = map[vk];
    // D3D refuses a color factor in an alpha slot; on the alpha channel it means the matching alpha factor.
    if (alpha) {
        switch (b) {
        case D3D11_BLEND_SRC_COLOR: return D3D11_BLEND_SRC_ALPHA;
        case D3D11_BLEND_INV_SRC_COLOR: return D3D11_BLEND_INV_SRC_ALPHA;
        case D3D11_BLEND_DEST_COLOR: return D3D11_BLEND_DEST_ALPHA;
        case D3D11_BLEND_INV_DEST_COLOR: return D3D11_BLEND_INV_DEST_ALPHA;
        default: break;
        }
    }
    return b;
}

D3D11_STENCIL_OP stencil_op(int vk) { return D3D11_STENCIL_OP(std::clamp(vk, 0, 7) + 1); }
D3D11_COMPARISON_FUNC compare(int vk) { return D3D11_COMPARISON_FUNC(std::clamp(vk, 0, 7) + 1); }

D3D11_DEPTH_STENCILOP_DESC stencil_side(const StencilSide &s)
{
    return {stencil_op(s.fail), stencil_op(s.depth_fail), stencil_op(s.pass), compare(s.compare)};
}

Mapped map_pipeline(const Pipeline &p, const std::vector<uint32_t> &vs_inputs)
{
    Mapped m;
    switch (p.topology) {
    case 0: m.topology = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST; m.type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT; m.vertices = 1; break;
    case 1: m.topology = D3D11_PRIMITIVE_TOPOLOGY_LINELIST; m.type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE; m.vertices = 2; break;
    case 2: m.topology = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP; m.type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE; m.vertices = 2; break;
    case 3: break;
    case 4: m.topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; break;
    case 5: m.notes.push_back("topology-fan-as-list"); break;
    case 6: m.topology = D3D11_PRIMITIVE_TOPOLOGY_LINELIST_ADJ; m.type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE; m.vertices = 4; break;
    case 7: m.topology = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ; m.type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE; m.vertices = 4; break;
    case 8: m.topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ; m.vertices = 6; break;
    case 9: m.topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ; m.vertices = 6; break;
    default: m.notes.push_back("topology-" + std::to_string(p.topology)); break;
    }

    m.raster.FillMode = p.polygon == 1 ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
    if (p.polygon == 2) m.notes.push_back("fill-point");
    switch (p.cull) {
    case 0: m.raster.CullMode = D3D11_CULL_NONE; break;
    case 1: m.raster.CullMode = D3D11_CULL_FRONT; break;
    case 2: m.raster.CullMode = D3D11_CULL_BACK; break;
    default: m.raster.CullMode = D3D11_CULL_NONE; m.notes.push_back("cull-front-and-back"); break;
    }
    m.raster.FrontCounterClockwise = p.front == 0;
    m.raster.DepthClipEnable = !p.clamp;

    m.depth.DepthEnable = FALSE;
    m.depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    m.depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
    m.depth.FrontFace = m.depth.BackFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
                                            D3D11_COMPARISON_ALWAYS};
    m.depth.StencilReadMask = m.depth.StencilWriteMask = 0xFF;
    if (p.depth) {
        const DepthState &d = *p.depth;
        m.depth.DepthEnable = d.test || d.write;
        if (!d.test && d.write) m.notes.push_back("depth-write-without-test");
        m.depth.DepthWriteMask = d.write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        m.depth.DepthFunc = d.test ? compare(d.compare) : D3D11_COMPARISON_ALWAYS;
        m.depth.StencilEnable = d.stencil;
        if (d.stencil) {
            m.depth.StencilReadMask = UINT8(d.front.compare_mask);
            m.depth.StencilWriteMask = UINT8(d.front.write_mask);
            m.depth.FrontFace = stencil_side(d.front);
            m.depth.BackFace = stencil_side(d.back);
        }
    }

    D3D11_RENDER_TARGET_BLEND_DESC &rt = m.blend.RenderTarget[0];
    rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_ZERO;
    rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (p.blend) {
        const BlendState &b = *p.blend;
        rt.BlendEnable = b.enable;
        rt.RenderTargetWriteMask = UINT8(b.write_mask & 0xF);
        if (b.enable) {
            rt.SrcBlend = blend_factor(b.src_color, false, m.notes);
            rt.DestBlend = blend_factor(b.dst_color, false, m.notes);
            rt.SrcBlendAlpha = blend_factor(b.src_alpha, true, m.notes);
            rt.DestBlendAlpha = blend_factor(b.dst_alpha, true, m.notes);
            rt.BlendOp = D3D11_BLEND_OP(std::clamp(b.color_op, 0, 4) + 1);
            rt.BlendOpAlpha = D3D11_BLEND_OP(std::clamp(b.alpha_op, 0, 4) + 1);
        }
    }

    m.rt = color_format(p.color_format, m.notes);
    m.ds = depth_format(p.zeta_format, m.notes);

    std::map<uint32_t, const VertexBinding *> bindings;
    for (const VertexBinding &b : p.bindings) bindings[b.binding] = &b;
    bool spare = false;
    for (uint32_t loc : vs_inputs) {
        auto a = std::find_if(p.attributes.begin(), p.attributes.end(),
                              [&](const VertexAttribute &x) { return x.location == loc; });
        D3D11_INPUT_ELEMENT_DESC e{"TEXCOORD", loc, DXGI_FORMAT_R32G32B32A32_FLOAT, kSpareSlot, 0,
                                   D3D11_INPUT_PER_VERTEX_DATA, 0};
        if (a != p.attributes.end() && a->binding < kSpareSlot) {
            e.Format = vertex_format(a->format, m.notes);
            e.InputSlot = a->binding;
            e.AlignedByteOffset = a->offset;
            auto b = bindings.find(a->binding);
            if (b != bindings.end() && b->second->rate == 1) {
                e.InputSlotClass = D3D11_INPUT_PER_INSTANCE_DATA;
                e.InstanceDataStepRate = 1;
            }
        } else {
            spare = true;
        }
        m.layout.push_back(e);
    }
    for (const VertexBinding &b : p.bindings)
        if (b.binding < kSpareSlot) m.buffers.push_back({b.binding, b.stride});
    if (spare) m.buffers.push_back({kSpareSlot, 0});
    std::sort(m.notes.begin(), m.notes.end());
    m.notes.erase(std::unique(m.notes.begin(), m.notes.end()), m.notes.end());
    return m;
}

D3D12_BLEND_DESC to12(const D3D11_BLEND_DESC &b)
{
    D3D12_BLEND_DESC o{};
    const D3D11_RENDER_TARGET_BLEND_DESC &s = b.RenderTarget[0];
    D3D12_RENDER_TARGET_BLEND_DESC &d = o.RenderTarget[0];
    d.BlendEnable = s.BlendEnable;
    d.SrcBlend = D3D12_BLEND(s.SrcBlend);
    d.DestBlend = D3D12_BLEND(s.DestBlend);
    d.BlendOp = D3D12_BLEND_OP(s.BlendOp);
    d.SrcBlendAlpha = D3D12_BLEND(s.SrcBlendAlpha);
    d.DestBlendAlpha = D3D12_BLEND(s.DestBlendAlpha);
    d.BlendOpAlpha = D3D12_BLEND_OP(s.BlendOpAlpha);
    d.LogicOp = D3D12_LOGIC_OP_NOOP;
    d.RenderTargetWriteMask = s.RenderTargetWriteMask;
    return o;
}

D3D12_RASTERIZER_DESC to12(const D3D11_RASTERIZER_DESC &r)
{
    D3D12_RASTERIZER_DESC o{};
    o.FillMode = D3D12_FILL_MODE(r.FillMode);
    o.CullMode = D3D12_CULL_MODE(r.CullMode);
    o.FrontCounterClockwise = r.FrontCounterClockwise;
    o.DepthClipEnable = r.DepthClipEnable;
    return o;
}

D3D12_DEPTH_STENCIL_DESC to12(const D3D11_DEPTH_STENCIL_DESC &s)
{
    D3D12_DEPTH_STENCIL_DESC o{};
    o.DepthEnable = s.DepthEnable;
    o.DepthWriteMask = D3D12_DEPTH_WRITE_MASK(s.DepthWriteMask);
    o.DepthFunc = D3D12_COMPARISON_FUNC(s.DepthFunc);
    o.StencilEnable = s.StencilEnable;
    o.StencilReadMask = s.StencilReadMask;
    o.StencilWriteMask = s.StencilWriteMask;
    auto side = [](const D3D11_DEPTH_STENCILOP_DESC &x) {
        return D3D12_DEPTH_STENCILOP_DESC{D3D12_STENCIL_OP(x.StencilFailOp), D3D12_STENCIL_OP(x.StencilDepthFailOp),
                                          D3D12_STENCIL_OP(x.StencilPassOp), D3D12_COMPARISON_FUNC(x.StencilFunc)};
    };
    o.FrontFace = side(s.FrontFace);
    o.BackFace = side(s.BackFace);
    return o;
}

constexpr UINT kTarget = 16;

struct Device {
    ~Device()
    {
        if (fence_event) CloseHandle(fence_event);
    }
    ComPtr<ID3D12Device> dev12;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D11Device> dev11;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D11Buffer> zero_vb, zero_cb, draw_cb;
    ComPtr<ID3D11Query> stats;
    ComPtr<ID3D12Fence> fence;
    HANDLE fence_event = nullptr;
    uint64_t fence_value = 0;
    std::map<DXGI_FORMAT, ComPtr<ID3D11RenderTargetView>> rtv;
    std::map<DXGI_FORMAT, ComPtr<ID3D11DepthStencilView>> dsv;
    std::string adapter;
    D3D_FEATURE_LEVEL level{};
};

std::string create_device(bool warp, Device &d)
{
    ComPtr<IDXGIFactory4> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return "CreateDXGIFactory1 hr=" + hex(hr);
    ComPtr<IDXGIAdapter> adapter;
    if (warp) hr = factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
    else hr = factory->EnumAdapters(0, &adapter);
    if (FAILED(hr)) return "adapter hr=" + hex(hr);
    DXGI_ADAPTER_DESC ad{};
    adapter->GetDesc(&ad);
    char name[128];
    snprintf(name, sizeof name, "%ls vendor 0x%04X device 0x%04X", ad.Description, ad.VendorId, ad.DeviceId);
    d.adapter = name;
    hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d.dev12));
    if (FAILED(hr)) return "D3D12CreateDevice hr=" + hex(hr);
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = d.dev12->CreateCommandQueue(&qd, IID_PPV_ARGS(&d.queue));
    if (FAILED(hr)) return "CreateCommandQueue hr=" + hex(hr);
    static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1};
    IUnknown *queues[] = {d.queue.Get()};
    hr = D3D11On12CreateDevice(d.dev12.Get(), 0, levels, 1, queues, 1, 0, &d.dev11, &d.ctx, &d.level);
    if (FAILED(hr)) return "D3D11On12CreateDevice 11_1 hr=" + hex(hr);

    D3D12_DESCRIPTOR_RANGE ranges[3] = {
        {D3D12_DESCRIPTOR_RANGE_TYPE_CBV, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, 0, 0, 0},
    };
    D3D12_ROOT_PARAMETER params[3]{};
    for (int i = 0; i < 3; i++) {
        params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[i].DescriptorTable = {1, &ranges[i]};
        params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_ROOT_SIGNATURE_DESC rd{3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    ComPtr<ID3DBlob> blob, err;
    hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
    if (FAILED(hr)) return "D3D12SerializeRootSignature hr=" + hex(hr);
    hr = d.dev12->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&d.root));
    if (FAILED(hr)) return "CreateRootSignature hr=" + hex(hr);

    static const std::vector<uint8_t> zeros(D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 16);
    D3D11_SUBRESOURCE_DATA init{zeros.data(), 0, 0};
    D3D11_BUFFER_DESC vb{UINT(zeros.size()), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
    D3D11_BUFFER_DESC cb{UINT(zeros.size()), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    D3D11_BUFFER_DESC dcb{4096, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    if (FAILED(hr = d.dev11->CreateBuffer(&vb, &init, &d.zero_vb))) return "zero vertex buffer hr=" + hex(hr);
    if (FAILED(hr = d.dev11->CreateBuffer(&cb, &init, &d.zero_cb))) return "zero constant buffer hr=" + hex(hr);
    if (FAILED(hr = d.dev11->CreateBuffer(&dcb, nullptr, &d.draw_cb))) return "draw constant buffer hr=" + hex(hr);
    D3D11_QUERY_DESC qs{D3D11_QUERY_PIPELINE_STATISTICS, 0};
    if (FAILED(hr = d.dev11->CreateQuery(&qs, &d.stats))) return "statistics query hr=" + hex(hr);
    if (FAILED(hr = d.dev12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&d.fence)))) return "fence hr=" + hex(hr);
    d.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return "";
}

ID3D11RenderTargetView *target_for(Device &d, DXGI_FORMAT f, std::vector<std::string> &notes)
{
    if (f == DXGI_FORMAT_UNKNOWN) return nullptr;
    auto it = d.rtv.find(f);
    if (it != d.rtv.end()) {
        if (!it->second) notes.push_back("rt-unsupported-" + std::to_string(int(f)));
        return it->second.Get();
    }
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11RenderTargetView> view;
    D3D11_TEXTURE2D_DESC td{kTarget, kTarget, 1, 1, f, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0};
    if (FAILED(d.dev11->CreateTexture2D(&td, nullptr, &tex)) ||
        FAILED(d.dev11->CreateRenderTargetView(tex.Get(), nullptr, &view))) {
        notes.push_back("rt-unsupported-" + std::to_string(int(f)));
        view.Reset();
    }
    d.rtv[f] = view;
    return view.Get();
}

ID3D11DepthStencilView *depth_for(Device &d, DXGI_FORMAT f, std::vector<std::string> &notes)
{
    if (f == DXGI_FORMAT_UNKNOWN) return nullptr;
    auto it = d.dsv.find(f);
    if (it != d.dsv.end()) {
        if (!it->second) notes.push_back("ds-unsupported-" + std::to_string(int(f)));
        return it->second.Get();
    }
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11DepthStencilView> view;
    D3D11_TEXTURE2D_DESC td{kTarget, kTarget, 1, 1, f, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_DEPTH_STENCIL, 0, 0};
    if (FAILED(d.dev11->CreateTexture2D(&td, nullptr, &tex)) ||
        FAILED(d.dev11->CreateDepthStencilView(tex.Get(), nullptr, &view))) {
        notes.push_back("ds-unsupported-" + std::to_string(int(f)));
        view.Reset();
    }
    d.dsv[f] = view;
    return view.Get();
}

struct Objects {
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11GeometryShader> gs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11DepthStencilState> depth;
    ID3D11RenderTargetView *rtv = nullptr;
    ID3D11DepthStencilView *dsv = nullptr;
    Mapped m;
};

std::string make_objects(Device &d, const Compiled &vs, const Compiled *gs, const Compiled &ps, Objects &o)
{
    HRESULT hr;
    if (FAILED(hr = d.dev11->CreateVertexShader(vs.dxbc.data(), vs.dxbc.size(), nullptr, &o.vs)))
        return "CreateVertexShader hr=" + hex(hr);
    if (gs && FAILED(hr = d.dev11->CreateGeometryShader(gs->dxbc.data(), gs->dxbc.size(), nullptr, &o.gs)))
        return "CreateGeometryShader hr=" + hex(hr);
    if (FAILED(hr = d.dev11->CreatePixelShader(ps.dxbc.data(), ps.dxbc.size(), nullptr, &o.ps)))
        return "CreatePixelShader hr=" + hex(hr);
    if (!o.m.layout.empty() &&
        FAILED(hr = d.dev11->CreateInputLayout(o.m.layout.data(), UINT(o.m.layout.size()), vs.dxbc.data(),
                                               vs.dxbc.size(), &o.layout)))
        return "CreateInputLayout hr=" + hex(hr);
    if (FAILED(hr = d.dev11->CreateBlendState(&o.m.blend, &o.blend))) return "CreateBlendState hr=" + hex(hr);
    if (FAILED(hr = d.dev11->CreateRasterizerState(&o.m.raster, &o.raster))) return "CreateRasterizerState hr=" + hex(hr);
    if (FAILED(hr = d.dev11->CreateDepthStencilState(&o.m.depth, &o.depth)))
        return "CreateDepthStencilState hr=" + hex(hr);
    o.rtv = target_for(d, o.m.rt, o.m.notes);
    o.dsv = depth_for(d, o.m.ds, o.m.notes);
    return "";
}

void bind_targets(Device &d, const Objects &o)
{
    ID3D11RenderTargetView *rtv = o.rtv;
    d.ctx->OMSetRenderTargets(rtv ? 1 : 0, rtv ? &rtv : nullptr, o.dsv);
    D3D11_VIEWPORT vp{0, 0, float(kTarget), float(kTarget), 0, 1};
    d.ctx->RSSetViewports(1, &vp);
    ID3D11Buffer *cbs[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT];
    std::fill(std::begin(cbs), std::end(cbs), d.zero_cb.Get());
    d.ctx->VSSetConstantBuffers(0, UINT(std::size(cbs)), cbs);
    d.ctx->GSSetConstantBuffers(0, UINT(std::size(cbs)), cbs);
    d.ctx->PSSetConstantBuffers(0, UINT(std::size(cbs)), cbs);
}

void bind_pipeline(Device &d, const Objects &o)
{
    d.ctx->IASetInputLayout(o.layout.Get());
    for (const VertexBuffer &b : o.m.buffers) {
        ID3D11Buffer *vb = d.zero_vb.Get();
        UINT offset = 0;
        d.ctx->IASetVertexBuffers(b.slot, 1, &vb, &b.stride, &offset);
    }
    d.ctx->IASetPrimitiveTopology(o.m.topology);
    d.ctx->VSSetShader(o.vs.Get(), nullptr, 0);
    d.ctx->GSSetShader(o.gs.Get(), nullptr, 0);
    d.ctx->PSSetShader(o.ps.Get(), nullptr, 0);
    static const float factor[4] = {1, 1, 1, 1};
    d.ctx->OMSetBlendState(o.blend.Get(), factor, 0xFFFFFFFF);
    d.ctx->RSSetState(o.raster.Get());
    d.ctx->OMSetDepthStencilState(o.depth.Get(), 0);
}

template <class T>
bool wait_query(Device &d, ID3D11Query *q, T &data, double limit_ms)
{
    double start = now_ms();
    while (d.ctx->GetData(q, &data, sizeof data, 0) != S_OK) {
        if (now_ms() - start > limit_ms) return false;
        Sleep(0);
    }
    return true;
}

struct PipelineRun {
    Device *d;
    const Compiled *vs, *gs, *ps;
    const Pipeline *p;
    std::string r12 = "ok", r11 = "ok", detail;
    HRESULT hr12 = S_OK;
    double ms12 = 0, ms_create = 0, ms_draw = 0, ms_wait = 0;
    uint64_t vs_invocations = 0;
    Mapped m;
};

void run_pipeline(PipelineRun &r)
{
    Device &d = *r.d;
    r.m = map_pipeline(*r.p, r.vs->input_locations);

    std::vector<D3D12_INPUT_ELEMENT_DESC> layout12;
    for (const D3D11_INPUT_ELEMENT_DESC &e : r.m.layout)
        layout12.push_back({e.SemanticName, e.SemanticIndex, e.Format, e.InputSlot, e.AlignedByteOffset,
                            D3D12_INPUT_CLASSIFICATION(e.InputSlotClass), e.InstanceDataStepRate});
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = d.root.Get();
    pd.VS = {r.vs->dxbc.data(), r.vs->dxbc.size()};
    if (r.gs) pd.GS = {r.gs->dxbc.data(), r.gs->dxbc.size()};
    pd.PS = {r.ps->dxbc.data(), r.ps->dxbc.size()};
    pd.BlendState = to12(r.m.blend);
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState = to12(r.m.raster);
    pd.DepthStencilState = to12(r.m.depth);
    pd.InputLayout = {layout12.data(), UINT(layout12.size())};
    pd.PrimitiveTopologyType = r.m.type;
    pd.NumRenderTargets = r.m.rt == DXGI_FORMAT_UNKNOWN ? 0 : 1;
    pd.RTVFormats[0] = r.m.rt;
    pd.DSVFormat = r.m.ds;
    pd.SampleDesc = {1, 0};
    ComPtr<ID3D12PipelineState> pso;
    double t0 = now_ms();
    r.hr12 = d.dev12->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso));
    r.ms12 = now_ms() - t0;
    if (FAILED(r.hr12)) r.r12 = "fail";

    Objects o;
    o.m = r.m;
    t0 = now_ms();
    std::string failed = make_objects(d, *r.vs, r.gs, *r.ps, o);
    r.ms_create = now_ms() - t0;
    r.m.notes = o.m.notes;
    if (!failed.empty()) {
        r.r11 = "create_fail";
        r.detail = failed;
        return;
    }
    bind_targets(d, o);
    bind_pipeline(d, o);
    t0 = now_ms();
    d.ctx->Begin(d.stats.Get());
    d.ctx->Draw(o.m.vertices, 0);
    d.ctx->End(d.stats.Get());
    d.ctx->Flush();
    r.ms_draw = now_ms() - t0;
    D3D11_QUERY_DATA_PIPELINE_STATISTICS stats{};
    t0 = now_ms();
    bool answered = wait_query(d, d.stats.Get(), stats, 5000);
    r.ms_wait = now_ms() - t0;
    r.vs_invocations = stats.VSInvocations;
    HRESULT removed = d.dev11->GetDeviceRemovedReason();
    if (FAILED(removed)) {
        r.r11 = "removed";
        r.detail = "GetDeviceRemovedReason hr=" + hex(removed);
    } else if (!answered) {
        r.r11 = "timeout";
    } else if (stats.VSInvocations == 0) {
        r.r11 = "dropped";
    }
    d.ctx->ClearState();
}

struct Guarded {
    PipelineRun *run;
    std::string cxx_exception;
};

void guarded_body(void *p)
{
    Guarded &g = *static_cast<Guarded *>(p);
    try {
        run_pipeline(*g.run);
    } catch (const std::exception &e) {
        g.cxx_exception = std::string("C++ exception: ") + e.what();
    } catch (...) {
        g.cxx_exception = "C++ exception of unknown type";
    }
}

DWORD seh_call(void (*fn)(void *), void *ctx)
{
    __try {
        fn(ctx);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
}

struct BenchResult {
    std::vector<double> frame_ms, wait_ms;
    double thread_cpu_ms = 0, process_cpu_ms = 0;
};

enum class Variant { Same, Switch, SwitchCb };
constexpr unsigned kDraws = 1000;
constexpr UINT kBenchCbSlot = 11;

BenchResult bench(Device &d, const Objects &a, const Objects &b, Variant v, unsigned frames)
{
    BenchResult r;
    bind_targets(d, a);
    bind_pipeline(d, a);
    double cpu0 = thread_cpu_ms(), proc0 = process_cpu_ms();
    for (unsigned f = 0; f < frames + 10; f++) {
        if (f == 10) {
            cpu0 = thread_cpu_ms();
            proc0 = process_cpu_ms();
        }
        double t0 = now_ms();
        const Objects *bound = &a;
        for (unsigned i = 0; i < kDraws; i++) {
            if (v != Variant::Same && i % 2 == 0) {
                bound = (i / 2) % 2 ? &b : &a;
                bind_pipeline(d, *bound);
            }
            if (v == Variant::SwitchCb) {
                D3D11_MAPPED_SUBRESOURCE map;
                if (SUCCEEDED(d.ctx->Map(d.draw_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
                    float *c = static_cast<float *>(map.pData);
                    for (int k = 0; k < 256; k++) c[k] = float(i + k);
                    d.ctx->Unmap(d.draw_cb.Get(), 0);
                }
                ID3D11Buffer *cb = d.draw_cb.Get();
                d.ctx->VSSetConstantBuffers(kBenchCbSlot, 1, &cb);
            }
            d.ctx->Draw(bound->m.vertices, 0);
        }
        d.ctx->Flush();
        double t1 = now_ms();
        d.queue->Signal(d.fence.Get(), ++d.fence_value);
        d.fence->SetEventOnCompletion(d.fence_value, d.fence_event);
        WaitForSingleObject(d.fence_event, 5000);
        double t2 = now_ms();
        if (f >= 10) {
            r.frame_ms.push_back(t1 - t0);
            r.wait_ms.push_back(t2 - t1);
        }
    }
    r.thread_cpu_ms = thread_cpu_ms() - cpu0;
    r.process_cpu_ms = process_cpu_ms() - proc0;
    d.ctx->ClearState();
    return r;
}

std::wstring cache_path(const std::wstring &dir, const Shader &s)
{
    return dir + L"\\m0-dxbc\\" + std::wstring(s.glsl_sha256.begin(), s.glsl_sha256.end()) + L".bin";
}

void save_compiled(const std::wstring &dir, const Shader &s, const Compiled &c)
{
    CreateDirectoryW((dir + L"\\m0-dxbc").c_str(), nullptr);
    std::ofstream out(cache_path(dir, s), std::ios::binary);
    uint32_t n = uint32_t(c.input_locations.size());
    out.write(reinterpret_cast<const char *>(&n), 4);
    out.write(reinterpret_cast<const char *>(c.input_locations.data()), n * 4);
    out.write(reinterpret_cast<const char *>(c.dxbc.data()), std::streamsize(c.dxbc.size()));
}

std::optional<Compiled> load_compiled(const std::wstring &dir, const Shader &s)
{
    std::ifstream in(cache_path(dir, s), std::ios::binary);
    if (!in) return std::nullopt;
    std::vector<char> all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    uint32_t n = 0;
    if (all.size() < 4) return std::nullopt;
    memcpy(&n, all.data(), 4);
    if (all.size() < 4 + size_t(n) * 4) return std::nullopt;
    Compiled c;
    c.input_locations.resize(n);
    memcpy(c.input_locations.data(), all.data() + 4, n * 4);
    c.dxbc.assign(all.begin() + 4 + n * 4, all.end());
    return c;
}

const char *stage_name(Stage s)
{
    switch (s) {
    case Stage::Vertex: return "vs";
    case Stage::Geometry: return "gs";
    case Stage::Pixel: return "ps";
    }
    return "?";
}

std::string join(const std::vector<std::string> &v)
{
    std::string out;
    for (const std::string &s : v) out += (out.empty() ? "" : ",") + s;
    return out.empty() ? "-" : out;
}

void clear_shader_caches(Device &d, Tsv &tsv)
{
    ComPtr<ID3D12Device9> dev9;
    HRESULT hr = d.dev12.As(&dev9);
    tsv.row("cache", "QueryInterface(ID3D12Device9)", hex(hr));
    if (FAILED(hr)) return;
    static const struct {
        const char *name;
        D3D12_SHADER_CACHE_KIND_FLAGS kind;
    } kinds[] = {
        {"IMPLICIT_D3D_CACHE_FOR_DRIVER", D3D12_SHADER_CACHE_KIND_FLAG_IMPLICIT_D3D_CACHE_FOR_DRIVER},
        {"IMPLICIT_D3D_CONVERSIONS", D3D12_SHADER_CACHE_KIND_FLAG_IMPLICIT_D3D_CONVERSIONS},
        {"IMPLICIT_DRIVER_MANAGED", D3D12_SHADER_CACHE_KIND_FLAG_IMPLICIT_DRIVER_MANAGED},
        {"APPLICATION_MANAGED", D3D12_SHADER_CACHE_KIND_FLAG_APPLICATION_MANAGED},
    };
    for (const auto &k : kinds)
        tsv.row("cache", k.name, hex(dev9->ShaderCacheControl(k.kind, D3D12_SHADER_CACHE_CONTROL_FLAG_CLEAR)));
}

void summarize(const std::wstring &path, const LineSink &sink)
{
    std::ifstream in(path);
    std::string line;
    size_t shaders = 0, shaders_ok = 0, pipes = 0, pipes_ok = 0, pipes12_ok = 0;
    std::vector<double> total, glslang, cross, d3dc, pso;
    std::map<std::string, size_t> on12;
    while (std::getline(in, line)) {
        std::vector<std::string> f = fields(line);
        if (f.empty()) continue;
        if (f[0] == "shader" && f.size() >= 9) {
            shaders++;
            if (f[4] != "ok") continue;
            shaders_ok++;
            double g = std::stod(f[6]), c = std::stod(f[7]), x = std::stod(f[8]);
            glslang.push_back(g);
            cross.push_back(c);
            d3dc.push_back(x);
            total.push_back(g + c + x);
        } else if (f[0] == "pipeline" && f.size() >= 8) {
            pipes++;
            on12[f[2]]++;
            if (f[2] == "ok") pipes_ok++;
            if (f[3] == "ok") {
                pipes12_ok++;
                pso.push_back(std::stod(f[5]));
            }
        } else if (f[0] == "bench" && f.size() >= 9) {
            sink("m0 bench " + f[1] + ": " + f[4] + " us/draw median, " + f[6] + " us/frame median (cpu " + f[8] +
                 " us/draw thread)");
        }
    }
    sink("m0 shaders ok " + std::to_string(shaders_ok) + "/" + std::to_string(shaders) + ", chain median " +
         fixed(percentile(total, 0.5), 1) + " ms (glslang " + fixed(percentile(glslang, 0.5), 1) + ", spirv-cross " +
         fixed(percentile(cross, 0.5), 1) + ", d3dcompile " + fixed(percentile(d3dc, 0.5), 1) + "), p95 " +
         fixed(percentile(total, 0.95), 1) + " ms");
    std::string classes;
    for (const auto &[k, n] : on12)
        if (k != "ok") classes += " " + k + " " + std::to_string(n);
    sink("m0 pipelines on D3D11On12 ok " + std::to_string(pipes_ok) + "/" + std::to_string(pipes) +
         (classes.empty() ? "" : ";" + classes) + "; direct D3D12 ok " + std::to_string(pipes12_ok) + ", PSO median " +
         fixed(percentile(pso, 0.5), 2) + " ms");
}

}  // namespace

Outcome run_probe(const ProbeOptions &opt, const LineSink &sink)
{
    Parsed<Corpus> corpus = parse_corpus(opt.corpus);
    if (!corpus) {
        sink("m0 FAIL " + corpus.error());
        return Outcome::Failed;
    }
    const std::wstring path = opt.out_dir + L"\\m0.tsv";
    Resume resume = read_resume(path);
    Tsv tsv{_wfopen(path.c_str(), L"ab")};
    if (!tsv.f) {
        sink("m0 FAIL cannot open m0.tsv");
        return Outcome::Failed;
    }
    struct Closer {
        FILE *f;
        ~Closer() { fclose(f); }
    } closer{tsv.f};
    if (resume.ended) {
        sink("m0 already complete (m0.tsv has its end row)");
        summarize(path, sink);
        return Outcome::Done;
    }
    const Corpus &c = *corpus;
    if (resume.unfinished_begin) {
        const auto &[kind, id] = *resume.unfinished_begin;
        if (kind == "shader") {
            const Shader &s = c.shaders[size_t(id)];
            tsv.row("shader", id, stage_name(s.stage), s.glsl_sha256.substr(0, 12), "died", "-", 0, 0, 0, 0, 0,
                    "the process ended inside this shader");
            resume.shaders.insert(id);
        } else if (kind == "pipeline") {
            tsv.row("pipeline", id, "died", "-", "-", 0, 0, 0, 0, 0, "-", "the process ended inside this pipeline",
                    c.pipelines[size_t(id)].suite);
            resume.pipelines.insert(id);
        } else if (kind == "bench") {
            tsv.row("benchdone", "died");
            resume.bench = true;
        }
        sink("m0 resumed after a " + kind + " " + std::to_string(id) + " that ended the process");
    }
    int launch = resume.launches + 1;
    tsv.row("launch", launch);
    if (launch == 1) {
        tsv.row("meta", "corpus_shaders", c.shaders.size());
        tsv.row("meta", "corpus_pipelines", c.pipelines.size());
        tsv.row("meta", "corpus_bytes", opt.corpus.size());
    }

    Device dev;
    std::string err = create_device(opt.warp, dev);
    if (!err.empty()) {
        tsv.row("meta", "device_fail", err);
        sink("m0 FAIL device: " + err);
        return Outcome::Failed;
    }
    tsv.row("meta", "adapter", dev.adapter);
    sink("m0 launch " + std::to_string(launch) + " on " + dev.adapter + ", corpus " + std::to_string(c.shaders.size()) +
         " shaders, " + std::to_string(c.pipelines.size()) + " pipelines");
    clear_shader_caches(dev, tsv);

    chain_init();
    std::vector<std::optional<Compiled>> compiled(c.shaders.size());
    double reload_ms = 0;
    size_t reloaded = 0;
    for (size_t i = 0; i < c.shaders.size(); i++) {
        const Shader &s = c.shaders[i];
        if (resume.shaders.count(long(i))) {
            double t0 = now_ms();
            compiled[i] = load_compiled(opt.out_dir, s);
            reload_ms += now_ms() - t0;
            reloaded += compiled[i].has_value();
            if (!compiled[i]) {
                std::expected<Compiled, ChainError> again = compile(s);
                if (again) compiled[i] = std::move(*again);
            }
            continue;
        }
        tsv.row("begin", "shader", i);
        std::expected<Compiled, ChainError> r = compile(s);
        if (r) {
            tsv.row("shader", i, stage_name(s.stage), s.glsl_sha256.substr(0, 12), "ok", "-", fixed(r->ms.glslang_ms),
                    fixed(r->ms.cross_ms), fixed(r->ms.d3dcompile_ms), r->hlsl.size(), r->dxbc.size(),
                    r->rewrites.empty() ? "-" : "rewrite " + r->rewrites);
            save_compiled(opt.out_dir, s, *r);
            compiled[i] = std::move(*r);
        } else {
            tsv.row("shader", i, stage_name(s.stage), s.glsl_sha256.substr(0, 12), "fail", step_name(r.error().step),
                    fixed(r.error().ms.glslang_ms), fixed(r.error().ms.cross_ms), fixed(r.error().ms.d3dcompile_ms), 0, 0,
                    r.error().message);
        }
        if ((i + 1) % 100 == 0) sink("m0 shaders " + std::to_string(i + 1) + "/" + std::to_string(c.shaders.size()));
    }
    chain_finalize();
    if (reloaded) tsv.row("meta", "dxbc_reload", std::to_string(reloaded) + " shaders in " + fixed(reload_ms) + " ms");

    for (size_t j = 0; j < c.pipelines.size(); j++) {
        if (resume.pipelines.count(long(j))) continue;
        const Pipeline &p = c.pipelines[j];
        const std::optional<Compiled> &vs = compiled[size_t(p.vs)], &ps = compiled[size_t(p.ps)];
        const std::optional<Compiled> *gs = p.gs >= 0 ? &compiled[size_t(p.gs)] : nullptr;
        if (!vs || !ps || (gs && !*gs)) {
            tsv.row("pipeline", j, "shader_failed", "skipped", "-", 0, 0, 0, 0, 0, "-", "a stage's chain failed", p.suite);
            continue;
        }
        tsv.row("begin", "pipeline", j);
        PipelineRun run{&dev, &*vs, gs ? &**gs : nullptr, &*ps, &p};
        Guarded g{&run};
        DWORD code = seh_call(guarded_body, &g);
        if (code) {
            run.r11 = "thrown";
            run.detail = "structured exception " + hex(code);
        } else if (!g.cxx_exception.empty()) {
            run.r11 = "thrown";
            run.detail = g.cxx_exception;
        }
        tsv.row("pipeline", j, run.r11, run.r12, hex(run.hr12), fixed(run.ms12), fixed(run.ms_create),
                fixed(run.ms_draw), fixed(run.ms_wait), run.vs_invocations, join(run.m.notes),
                run.detail.empty() ? "-" : run.detail, p.suite.empty() ? "-" : p.suite);
        if ((j + 1) % 200 == 0) sink("m0 pipelines " + std::to_string(j + 1) + "/" + std::to_string(c.pipelines.size()));
        if (run.r11 == "thrown" || run.r11 == "removed") {
            if (FAILED(dev.dev11->GetDeviceRemovedReason()) || code) {
                tsv.row("relaunch", j, run.r11);
                sink("m0 relaunch: pipeline " + std::to_string(j) + " " + run.r11 + " (" + run.detail + ")");
                return Outcome::Relaunch;
            }
        }
    }

    if (!resume.bench) {
        tsv.row("begin", "bench", 0);
        std::vector<size_t> ok;
        {
            std::ifstream in(path);
            std::string line;
            while (std::getline(in, line)) {
                std::vector<std::string> f = fields(line);
                if (f.size() > 2 && f[0] == "pipeline" && f[2] == "ok") ok.push_back(size_t(std::stol(f[1])));
            }
        }
        // Triangle lists first; xemu draws every primitive but points through a geometry shader, so the pair has one.
        std::optional<size_t> a, b;
        for (bool triangles : {true, false}) {
            a.reset();
            b.reset();
            for (size_t j : ok) {
                const Pipeline &p = c.pipelines[j];
                if ((triangles && p.topology != 3) || p.color_format == 0) continue;
                if (!a) a = j;
                else if (p.vs != c.pipelines[*a].vs || p.ps != c.pipelines[*a].ps) {
                    b = j;
                    break;
                }
            }
            if (b) break;
        }
        if (!a || !b) {
            tsv.row("benchdone", "no two accepted pipelines with different shaders");
            sink("m0 bench not run: no two accepted pipelines with different shaders");
        } else {
            Objects oa, ob;
            const Pipeline &pa = c.pipelines[*a], &pb = c.pipelines[*b];
            oa.m = map_pipeline(pa, compiled[size_t(pa.vs)]->input_locations);
            ob.m = map_pipeline(pb, compiled[size_t(pb.vs)]->input_locations);
            auto gs_of = [&](const Pipeline &p) { return p.gs >= 0 ? &*compiled[size_t(p.gs)] : nullptr; };
            std::string fa = make_objects(dev, *compiled[size_t(pa.vs)], gs_of(pa), *compiled[size_t(pa.ps)], oa);
            std::string fb = make_objects(dev, *compiled[size_t(pb.vs)], gs_of(pb), *compiled[size_t(pb.ps)], ob);
            if (!fa.empty() || !fb.empty()) {
                tsv.row("benchdone", "objects failed: " + fa + " " + fb);
            } else {
                tsv.row("meta", "bench_pipelines", std::to_string(*a) + "," + std::to_string(*b));
                static const struct {
                    Variant v;
                    const char *name;
                } variants[] = {{Variant::Same, "same"}, {Variant::Switch, "switch"}, {Variant::SwitchCb, "switch_cb"}};
                for (const auto &[v, name] : variants) {
                    BenchResult r = bench(dev, oa, ob, v, opt.bench_frames);
                    std::vector<double> draw_us;
                    for (double ms : r.frame_ms) draw_us.push_back(ms * 1000.0 / kDraws);
                    double draws = double(r.frame_ms.size()) * kDraws;
                    tsv.row("bench", name, r.frame_ms.size(), kDraws, fixed(percentile(draw_us, 0.5), 2),
                            fixed(percentile(draw_us, 0.95), 2), fixed(percentile(r.frame_ms, 0.5) * 1000.0, 0),
                            fixed(percentile(r.frame_ms, 0.95) * 1000.0, 0), fixed(r.thread_cpu_ms * 1000.0 / draws, 2),
                            fixed(r.process_cpu_ms * 1000.0 / draws, 2), fixed(percentile(r.wait_ms, 0.5), 2));
                }
                tsv.row("benchdone", "ok");
            }
        }
    }
    tsv.row("end");
    summarize(path, sink);
    return Outcome::Done;
}

}  // namespace xbrt::m0
