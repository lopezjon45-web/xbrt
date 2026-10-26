#include "shell/uwp/gfx.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <d3d11_1.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Display.Core.h>

namespace xbrt::gfx {

namespace {

constexpr unsigned kBuffers = 2;

struct Gfx {
    winrt::com_ptr<ID3D12Device> dev12;
    winrt::com_ptr<ID3D12CommandQueue> queue;
    winrt::com_ptr<ID3D11Device> dev11;
    winrt::com_ptr<ID3D11DeviceContext> ctx;
    winrt::com_ptr<ID3D11On12Device> on12;
    winrt::com_ptr<IDXGISwapChain3> swap;
    winrt::com_ptr<ID3D11Resource> wrapped[kBuffers];
    winrt::com_ptr<ID3D11Texture2D> text;
    unsigned width = 0, height = 0, hz = 60;
    std::string device_line = "device: not created";
};

Gfx g;

const char *level_name(D3D_FEATURE_LEVEL l)
{
    switch (l) {
    case D3D_FEATURE_LEVEL_12_1: return "12_1";
    case D3D_FEATURE_LEVEL_12_0: return "12_0";
    case D3D_FEATURE_LEVEL_11_1: return "11_1";
    case D3D_FEATURE_LEVEL_11_0: return "11_0";
    default: return "below 11_0";
    }
}

bool ok(HRESULT hr, const char *step, std::string &log)
{
    char buf[128];
    snprintf(buf, sizeof buf, "gfx %s hr=0x%08lx\n", step, (unsigned long)hr);
    log += buf;
    if (FAILED(hr)) g.device_line = std::string("device: ") + step + " failed " + buf;
    return SUCCEEDED(hr);
}

}  // namespace

DisplayMode display_mode()
{
    using winrt::Windows::Graphics::Display::Core::HdmiDisplayInformation;
    DisplayMode m{1280, 720, 60, false};
    try {
        if (auto info = HdmiDisplayInformation::GetForCurrentView()) {
            auto mode = info.GetCurrentDisplayMode();
            m.width = std::min(mode.ResolutionWidthInRawPixels(), 1920u);
            m.height = std::min(mode.ResolutionHeightInRawPixels(), 1080u);
            long hz = std::lround(mode.RefreshRate());
            m.hz = hz > 0 ? unsigned(hz) : 60;
            m.from_hdmi = true;
        }
    } catch (winrt::hresult_error const &) {
    }
    return m;
}

bool create(IUnknown *core_window, const DisplayMode &mode, std::string &log)
{
    if (!ok(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(g.dev12.put())), "d3d12 device", log))
        return false;
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (!ok(g.dev12->CreateCommandQueue(&qd, IID_PPV_ARGS(g.queue.put())), "d3d12 queue", log)) return false;

    static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    IUnknown *queues[] = {g.queue.get()};
    D3D_FEATURE_LEVEL level = {};
    HRESULT hr = D3D11On12CreateDevice(g.dev12.get(), D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, queues, 1, 0,
                                       g.dev11.put(), g.ctx.put(), &level);
    if (!ok(hr, "d3d11on12 device", log)) return false;
    log += std::string("gfx d3d11on12 level ") + level_name(level) + "\n";
    g.device_line = std::string("device ") + level_name(level);
    if (level < D3D_FEATURE_LEVEL_11_1) {
        g.device_line += " (below 11_1)";
        return false;
    }
    g.on12 = g.dev11.as<ID3D11On12Device>();

    winrt::com_ptr<IDXGIFactory4> factory;
    if (!ok(CreateDXGIFactory1(IID_PPV_ARGS(factory.put())), "dxgi factory", log)) return false;
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = mode.width;
    sd.Height = mode.height;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kBuffers;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Scaling = DXGI_SCALING_STRETCH;
    winrt::com_ptr<IDXGISwapChain1> swap1;
    if (!ok(factory->CreateSwapChainForCoreWindow(g.queue.get(), core_window, &sd, nullptr, swap1.put()),
            "swap chain for core window", log))
        return false;
    winrt::com_ptr<IDXGISwapChain3> swap = swap1.as<IDXGISwapChain3>();

    for (UINT i = 0; i < kBuffers; i++) {
        winrt::com_ptr<ID3D12Resource> back;
        if (!ok(swap->GetBuffer(i, IID_PPV_ARGS(back.put())), "back buffer", log)) return false;
        D3D11_RESOURCE_FLAGS flags = {D3D11_BIND_RENDER_TARGET, 0, 0, 0};
        if (!ok(g.on12->CreateWrappedResource(back.get(), &flags, D3D12_RESOURCE_STATE_PRESENT,
                                              D3D12_RESOURCE_STATE_PRESENT, IID_PPV_ARGS(g.wrapped[i].put())),
                "wrap back buffer", log))
            return false;
    }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = mode.width;
    td.Height = mode.height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    if (!ok(g.dev11->CreateTexture2D(&td, nullptr, g.text.put()), "status texture", log)) return false;
    g.width = mode.width;
    g.height = mode.height;
    g.hz = mode.hz;
    g.swap = std::move(swap);
    char buf[128];
    snprintf(buf, sizeof buf, "gfx ready %ux%u at %u Hz (%s)\n", g.width, g.height, g.hz,
             mode.from_hdmi ? "hdmi mode" : "fallback");
    log += buf;
    return true;
}

std::string device_line() { return g.device_line; }

void present(const uint8_t *rgba, bool changed)
{
    if (!g.swap) {
        Sleep(16);
        return;
    }
    if (changed) g.ctx->UpdateSubresource(g.text.get(), 0, nullptr, rgba, g.width * 4, 0);
    UINT i = g.swap->GetCurrentBackBufferIndex();
    ID3D11Resource *r = g.wrapped[i].get();
    g.on12->AcquireWrappedResources(&r, 1);
    g.ctx->CopyResource(r, g.text.get());
    g.on12->ReleaseWrappedResources(&r, 1);
    g.ctx->Flush();
    g.swap->Present(1, 0);
}

}  // namespace xbrt::gfx
