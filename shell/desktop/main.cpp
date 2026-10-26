#include "host/memory.h"
#include "host/probes.h"
#include "host/session.h"
#include "tests/host/cpu_tests.h"

#include <windows.h>

#include <d3d11on12.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {

FILE *g_run;

void line(const std::string &s)
{
    std::fputs(s.c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
    if (g_run) {
        std::fputs(s.c_str(), g_run);
        std::fputc('\n', g_run);
        std::fflush(g_run);
    }
}

std::string hex(HRESULT hr)
{
    char buf[16];
    snprintf(buf, sizeof buf, "0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

std::string device_line()
{
    ComPtr<IDXGIFactory4> factory;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return "device FAIL CreateDXGIFactory2 hr=" + hex(hr);
    ComPtr<IDXGIAdapter> warp;
    hr = factory->EnumWarpAdapter(IID_PPV_ARGS(&warp));
    if (FAILED(hr)) return "device FAIL EnumWarpAdapter hr=" + hex(hr);
    ComPtr<ID3D12Device> device12;
    hr = D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device12));
    if (FAILED(hr)) return "device FAIL D3D12CreateDevice hr=" + hex(hr);
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    hr = device12->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));
    if (FAILED(hr)) return "device FAIL CreateCommandQueue hr=" + hex(hr);
    const D3D_FEATURE_LEVEL want = D3D_FEATURE_LEVEL_11_1;
    IUnknown *queues[] = {queue.Get()};
    ComPtr<ID3D11Device> device11;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_9_1;
    hr = D3D11On12CreateDevice(device12.Get(), D3D11_CREATE_DEVICE_BGRA_SUPPORT, &want, 1, queues, 1, 0, &device11, &context, &got);
    if (FAILED(hr)) return "device FAIL D3D11On12CreateDevice hr=" + hex(hr);
    char buf[32];
    snprintf(buf, sizeof buf, "device %u_%u warp", unsigned(got >> 12), unsigned((got >> 8) & 0xF));
    return buf;
}

std::wstring wide(std::u16string_view s) { return std::wstring(reinterpret_cast<const wchar_t *>(s.data()), s.size()); }

std::u16string narrow16(const std::wstring &s) { return std::u16string(reinterpret_cast<const char16_t *>(s.data()), s.size()); }

struct DesktopServices final : xbrt::ShellServices {
    void stage(xbrt::BootStage, std::string_view l) override { line(std::string(l)); }
    void fail(xbrt::BootStage s, std::string_view l) override
    {
        line(std::string(xbrt::boot_stage_name(s)) + " FAIL " + std::string(l));
    }
    std::unique_ptr<xbrt::Source> open_disc(std::u16string_view path, std::string &error) override
    {
        struct File final : xbrt::Source {
            mutable std::ifstream in;
            uint64_t bytes = 0;
            bool read_at(uint64_t offset, std::span<uint8_t> out) const override
            {
                if (offset > bytes || out.size() > bytes - offset) return false;
                in.clear();
                in.seekg(std::streamoff(offset));
                in.read(reinterpret_cast<char *>(out.data()), std::streamsize(out.size()));
                return bool(in);
            }
            uint64_t size() const override { return bytes; }
        };
        auto f = std::make_unique<File>();
        std::filesystem::path p(wide(path));
        std::error_code ec;
        f->bytes = std::filesystem::file_size(p, ec);
        f->in.open(p, std::ios::binary);
        if (ec || !f->in) {
            error = "cannot open " + p.string();
            return nullptr;
        }
        return f;
    }
    std::vector<std::u16string> list_discs(std::u16string_view folder, std::string &error) override
    {
        std::vector<std::u16string> out;
        std::error_code ec;
        for (const auto &e : std::filesystem::directory_iterator(wide(folder), ec)) {
            std::wstring ext = e.path().extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
            if (e.is_regular_file() && (ext == L".iso" || ext == L".xiso")) out.push_back(narrow16(e.path().wstring()));
        }
        if (ec) error = "cannot list " + std::filesystem::path(wide(folder)).string() + ": " + ec.message();
        std::sort(out.begin(), out.end());
        return out;
    }
    std::u16string data_root() override { return narrow16(std::filesystem::current_path().wstring()); }
};

}  // namespace

// xbrt_desktop.exe [--survey <folder> | --cpu-tests]: the stages and probes; with --survey, the survey over a folder
// of images instead, writing survey.tsv beside run.txt; with --cpu-tests, the CPU core's tests, writing cpu-tests.txt.
int main(int argc, char **argv)
{
    bool cpu_tests = argc == 2 && std::string(argv[1]) == "--cpu-tests";
    const char *log_name = cpu_tests ? "cpu-tests.txt" : "run.txt";
    g_run = std::fopen(log_name, "w");
    if (!g_run) std::fprintf(stderr, "cannot open %s\n", log_name);
    if (argc == 3 && std::string(argv[1]) == "--survey") {
        DesktopServices services;
        std::string folder = argv[2];
        xbrt::Parsed<xbrt::SurveyTotals> totals =
            xbrt::run_survey(std::u16string(folder.begin(), folder.end()), services, [](std::string_view l) { line(std::string(l)); });
        if (!totals) {
            line("survey FAIL " + totals.error());
            line("END");
            return 1;
        }
        line("survey ok images=" + std::to_string(totals->images) + " parsed=" + std::to_string(totals->parsed) +
             " ready=" + std::to_string(totals->ready) + " named-gaps=" + std::to_string(totals->named_gaps) +
             " unparsed=" + std::to_string(totals->unparsed) + " builds=" + std::to_string(totals->distinct_builds));
        line("END");
        return 0;
    }

    xbrt::Parsed<std::unique_ptr<xbrt::GuestMemory>> memory = xbrt::GuestMemory::reserve(xbrt::RamSize::Retail64);
    if (!memory) {
        line(memory.error());
        line("END");
        return 1;
    }
    for (const std::string &s : (*memory)->log()) line(s);

    std::string identity = (*memory)->identity_check();
    line(identity);
    bool ok = identity.starts_with("identity ok");

    if (cpu_tests) {
        ok = xbrt::run_cpu_tests(**memory, [](const std::string &l) { line(l); }) && ok;
        line("END");
        if (g_run) std::fclose(g_run);
        return ok ? 0 : 1;
    }

    line(device_line());

    ok = xbrt::run_cpu_probes(**memory, [](const xbrt::ProbeLine &p) { line(xbrt::probe_text(p)); }) && ok;
    line("END");
    if (g_run) std::fclose(g_run);
    return ok ? 0 : 1;
}
