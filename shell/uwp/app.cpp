#include "core/launch.h"
#include "core/xbe.h"
#include "host/m0/probe.h"
#include "host/memory.h"
#include "host/probes.h"
#include "host/session.h"
#include "shell/uwp/disc_file.h"
#include "shell/uwp/gfx.h"
#include "shell/uwp/run_log.h"
#include "shell/uwp/status.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <winrt/base.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Core.h>

using namespace winrt;
using namespace Windows::ApplicationModel::Activation;
using namespace Windows::ApplicationModel::Core;
using namespace Windows::Foundation;
using namespace Windows::Gaming::Input;
using namespace Windows::UI::Core;
using xbrt::LineColour;

namespace {

std::unique_ptr<xbrt::GuestMemory> g_memory;
std::string g_reserve_error;
std::wstring g_local_state;
xbrt::StatusScreen g_screen;
std::atomic<bool> g_boot_running{false};
constexpr std::u16string_view kLibrary = u"E:\\Xbox";   // where the hub and the game library service file Xbox games

struct Failure {
    std::string kind, line;
};
std::mutex g_failure_lock;
std::optional<Failure> g_first_failure;
// Set by the boot thread when the guest itself ended the session (result=ok); the pad loop then leaves for the hub.
std::atomic<bool> g_guest_left{false};
// The Devices stage has passed: the title's pad exists and the pad line can be shown.
std::atomic<bool> g_devices_attached{false};
bool g_m0 = false;

std::string utf8(std::u16string_view s)
{
    return to_string(std::wstring_view(reinterpret_cast<const wchar_t *>(s.data()), s.size()));
}

std::u16string utf16(std::wstring_view s)
{
    return std::u16string(reinterpret_cast<const char16_t *>(s.data()), s.size());
}

void report(std::string_view kind, const std::string &line, LineColour colour, std::string_view replaces = {})
{
    if (replaces.empty())
        g_screen.add(line, colour);
    else
        g_screen.replace(replaces, line, colour);
    xbrt::run_log::run(line);
    if (colour != LineColour::Red) return;
    std::lock_guard<std::mutex> g(g_failure_lock);
    if (!g_first_failure) g_first_failure = Failure{std::string(kind), line};
}

struct Link {
    std::optional<xbrt::LaunchRequest> request;
    std::string line;
    LineColour colour;
};

Link link_from_uri(Uri const &uri, const std::string &source)
{
    xbrt::QueryPairs pairs;
    for (IWwwFormUrlDecoderEntry const &e : uri.QueryParsed())
        pairs.emplace_back(utf16(e.Name()), utf16(e.Value()));
    xbrt::Parsed<xbrt::LaunchRequest> request = xbrt::parse_launch(pairs);
    if (!request) return {std::nullopt, "link FAIL " + request.error() + " (" + source + ")", LineColour::Red};
    const std::u16string &disc = request->disc_path;
    std::string what = !request->survey_folder.empty() ? "survey " + utf8(request->survey_folder)
                       : !request->dir_path.empty()    ? "dir " + utf8(request->dir_path)
                                                       : utf8(disc.substr(disc.find_last_of(u'\\') + 1));
    std::string line = "link " + to_string(uri.SchemeName()) + ": " + what + " (" + source + ")" +
                       (request->home.empty() ? ", home none (LocalState)" : ", home " + utf8(request->home));
    return {std::move(*request), line, LineColour::Green};
}

std::optional<std::string> read_launch_txt()
{
    FILE *f = _wfopen((g_local_state + L"\\launch.txt").c_str(), L"rb");
    if (!f) return std::nullopt;
    char buf[4096];
    if (!fgets(buf, sizeof buf, f)) buf[0] = 0;
    fclose(f);
    std::string line = buf;
    return line.substr(0, line.find_first_of("\r\n"));
}

Link resolve_link(IActivatedEventArgs const &args)
{
    bool protocol = args.Kind() == ActivationKind::Protocol;
    std::string source = protocol ? "protocol" : "launch.txt";
    try {
        if (protocol) return link_from_uri(args.as<ProtocolActivatedEventArgs>().Uri(), source);
        std::optional<std::string> text = read_launch_txt();
        if (!text) return {std::nullopt, "link none: started without a link, listing " + utf8(kLibrary), LineColour::White};
        return link_from_uri(Uri(to_hstring(*text)), source);
    } catch (hresult_error const &e) {
        char code[32];
        snprintf(code, sizeof code, " (0x%08x)", unsigned(e.code().value));
        return {std::nullopt, "link FAIL " + source + ": " + to_string(e.message()) + code, LineColour::Red};
    }
}

struct Services final : xbrt::ShellServices {
    void stage(xbrt::BootStage s, std::string_view line) override
    {
        report(xbrt::boot_stage_name(s), std::string(line), LineColour::Green);
        if (s == xbrt::BootStage::Devices) g_devices_attached = true;
        if (s == xbrt::BootStage::Start) g_screen.add("hold View and Menu to return to the hub", LineColour::White);
    }
    void fail(xbrt::BootStage s, std::string_view line) override
    {
        std::string_view name = xbrt::boot_stage_name(s);
        report(name, std::string(name) + " FAIL " + std::string(line), LineColour::Red);
    }
    std::unique_ptr<xbrt::Source> open_disc(std::u16string_view path, std::string &error) override
    {
        return xbrt::open_disc_file(path, error);
    }
    std::vector<std::u16string> list_discs(std::u16string_view folder, std::string &error) override
    {
        return xbrt::list_disc_files(folder, error);
    }
    // The lab programs' own lines are shown as well: they are how a lab reports what the guest saw.
    void log(std::string_view line) override
    {
        xbrt::run_log::run(line);
        constexpr std::string_view kLab = "guest: lab ";
        if (line.starts_with(kLab)) g_screen.add(std::string(line.substr(7)), LineColour::White);
    }
    void counters(std::string_view line) override { g_screen.replace("counters ", line, LineColour::Grey); }
    std::u16string data_root() override { return utf16(g_local_state); }
};

// The game link: the session boots through Start and runs to the guest's exit or its first wall. A stop stays on
// the screen in red until B; the hub's link carries the Missing's kind and text.
void play(const xbrt::LaunchRequest &request)
{
    if (!g_memory) {
        report("start", "start FAIL not run: the reservation failed", LineColour::Red);
        return;
    }
    Services services;
    std::expected<xbrt::Session, xbrt::BootError> session = xbrt::Session::boot(request, *g_memory, services);
    if (!session) return;
    ULONGLONG started = GetTickCount64();
    xbrt::ExitReason why = session->run();
    if (why == xbrt::ExitReason::GuestExited) {
        std::string line = "every guest thread ended after " + std::to_string((GetTickCount64() - started) / 1000) + " s";
        report("Feature", "stop Feature: " + line, LineColour::Red);
    }
    if (const xbrt::Missing *stop = session->stop()) {
        std::string line = xbrt::missing_line(*stop);
        g_screen.add(line, LineColour::Red);
        xbrt::run_log::run("stop " + line + " (crash.txt)");
        std::lock_guard<std::mutex> g(g_failure_lock);
        if (!g_first_failure) g_first_failure = Failure{std::string(xbrt::missing_kind_name(stop->kind)), stop->what};
    }
    xbrt::run_log::run("exit " + std::string(xbrt::exit_reason_name(why)));
    if (why != xbrt::ExitReason::Fatal) g_guest_left = true;
}

void survey(const xbrt::LaunchRequest &request)
{
    Services services;
    xbrt::Parsed<xbrt::SurveyTotals> totals = xbrt::run_survey(request.survey_folder, services, [](std::string_view l) {
        g_screen.replace("survey ", std::string(l), LineColour::Grey);
        xbrt::run_log::run(l);
    });
    if (!totals) {
        report("survey", "survey FAIL " + totals.error(), LineColour::Red);
        return;
    }
    const xbrt::SurveyTotals &t = *totals;
    report("survey",
           "survey ok " + std::to_string(t.parsed) + "/" + std::to_string(t.images) + " parsed, " +
               std::to_string(t.ready) + " ready, " + std::to_string(t.named_gaps) + " named gaps, " +
               std::to_string(t.unparsed) + " unparsed, " + std::to_string(t.distinct_builds) + " XDK builds",
           t.parsed ? LineColour::Green : LineColour::Red, "survey ");
    for (size_t i = 0; i < t.no_bridge_by_titles.size() && i < 8; i++) {
        const auto &[o, n] = t.no_bridge_by_titles[i];
        report("survey",
               "  no bridge: " + std::to_string(o.v) + " " + std::string(xbrt::kernel_export_name(o)) + " blocks " +
                   std::to_string(n) + " title" + (n == 1 ? "" : "s"),
               LineColour::White);
    }
}

void run_probes()
{
    if (!g_memory) {
        report("probe", "probe FAIL not run: the reservation failed", LineColour::Red);
        return;
    }
    xbrt::run_cpu_probes(*g_memory, [](const xbrt::ProbeLine &p) {
        std::string text = xbrt::probe_text(p);
        switch (p.state) {
        case xbrt::ProbeLine::State::Begin:
            g_screen.add(text, LineColour::Grey);
            xbrt::run_log::run(text);
            return;
        case xbrt::ProbeLine::State::Ok: report("probe", text, LineColour::Green, "BEGIN probe " + p.name); return;
        case xbrt::ProbeLine::State::Fail: report("probe", text, LineColour::Red, "BEGIN probe " + p.name); return;
        }
    });
}

// Started from the tile with no link: the game list from E:\Xbox, so the app is usable without the hub
// (INTEGRATION.md section 1). Written to LocalState\library.txt too, so console scripts take names from it.
void list_library()
{
    std::string error;
    std::vector<std::u16string> discs = xbrt::list_disc_files(kLibrary, error);
    if (!error.empty()) {
        report("library", "library FAIL " + utf8(kLibrary) + ": " + error, LineColour::Red);
        return;
    }
    report("library", "library " + utf8(kLibrary) + ": " + std::to_string(discs.size()) + " images", LineColour::Green);
    FILE *f = _wfopen((g_local_state + L"\\library.txt").c_str(), L"wb");
    for (const std::u16string &d : discs) {
        std::string name = utf8(d.substr(d.find_last_of(u'\\') + 1));
        g_screen.add("  " + name, LineColour::White);
        if (f) fprintf(f, "%s\n", utf8(d).c_str());
    }
    if (f) fclose(f);
}

void run_m0()
{
    std::wstring corpus_path =
        std::wstring(Windows::ApplicationModel::Package::Current().InstalledLocation().Path()) + L"\\m0-corpus.txt";
    FILE *f = _wfopen(corpus_path.c_str(), L"rb");
    if (!f) {
        report("m0", "m0 FAIL no m0-corpus.txt in the package", LineColour::Red);
        return;
    }
    xbrt::m0::ProbeOptions options;
    char buf[65536];
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) options.corpus.append(buf, n);
    fclose(f);
    options.out_dir = g_local_state;
    xbrt::m0::Outcome outcome = xbrt::m0::run_probe(options, [](const std::string &l) {
        report("m0", l, l.starts_with("m0 FAIL") ? LineColour::Red : LineColour::White);
    });
    if (outcome == xbrt::m0::Outcome::Relaunch) xbrt::run_log::run("m0 relaunch");
    if (outcome == xbrt::m0::Outcome::Done) report("m0", "m0 done", LineColour::Green);
}

void boot(Link link)
{
    if (g_m0) {
        report("link", link.line, link.colour);
        run_m0();
        xbrt::run_log::run("END");
        g_screen.add("done. B returns to the hub", LineColour::White);
        g_boot_running = false;
        return;
    }
    if (g_memory) {
        const std::vector<std::string> &steps = g_memory->log();
        for (size_t i = 0; i + 1 < steps.size(); i++) xbrt::run_log::run(steps[i]);
        report("reserve", steps.back(), LineColour::Green);
        std::string identity = g_memory->identity_check();
        report("identity", identity, identity.starts_with("identity ok") ? LineColour::Green : LineColour::Red);
    } else {
        size_t last = g_reserve_error.find_last_of('\n');
        if (last != std::string::npos) xbrt::run_log::run(g_reserve_error.substr(0, last));
        report("reserve", g_reserve_error.substr(last + 1), LineColour::Red);
        report("identity", "identity FAIL not run: the reservation failed", LineColour::Red);
    }

    report("link", link.line, link.colour);
    if (!link.request) list_library();
    if (link.request && !link.request->survey_folder.empty()) {
        survey(*link.request);
    } else if (link.request) {
        play(*link.request);
        xbrt::run_log::run("END");
        g_screen.add(g_guest_left ? "done. returning to the hub" : "done. B returns to the hub", LineColour::White);
        g_boot_running = false;
        return;
    }

    std::string device = xbrt::gfx::device_line();
    report("device", device, device.starts_with("device 11_1") ? LineColour::Green : LineColour::Red);
    run_probes();
    xbrt::run_log::run("END");
    g_screen.add("done. B returns to the hub", LineColour::White);
    g_boot_running = false;
}

// Any pad holds all of `buttons`.
bool held(GamepadButtons buttons)
{
    for (Gamepad const &pad : Gamepad::Gamepads())
        if ((pad.GetCurrentReading().Buttons & buttons) == buttons) return true;
    return false;
}

struct App : implements<App, IFrameworkViewSource, IFrameworkView> {
    CoreWindow window_{nullptr};
    std::optional<Link> link_;
    bool gfx_tried_ = false;
    bool boot_started_ = false;
    bool closed_ = false;
    bool left_ = false;

    IFrameworkView CreateView() { return *this; }

    void Initialize(CoreApplicationView const &view)
    {
        g_local_state = Windows::Storage::ApplicationData::Current().LocalFolder().Path();
        xbrt::run_log::open(g_local_state);
        auto v = Windows::ApplicationModel::Package::Current().Id().Version();
        char line[96];
        snprintf(line, sizeof line, "xbrt %u.%u.%u.%u", v.Major, v.Minor, v.Build, v.Revision);
        xbrt::run_log::shell(line);
        view.Activated([this](auto &&, IActivatedEventArgs const &args) { on_activated(args); });
    }

    void Load(hstring const &) {}
    void Uninitialize() {}

    void SetWindow(CoreWindow const &w)
    {
        window_ = w;
        window_.Closed([this](auto &&, auto &&) {
            closed_ = true;
            xbrt::run_log::shell("window closed");
        });
        // On the console an unhandled B is a system back, which can send the app away before the pad poll sees the
        // press and launches the return link.
        try {
            SystemNavigationManager::GetForCurrentView().BackRequested(
                [](auto &&, BackRequestedEventArgs const &e) { e.Handled(true); });
        } catch (hresult_error const &e) {
            xbrt::run_log::shell("BackRequested not handled: " + to_string(e.message()));
        }
    }

    void on_activated(IActivatedEventArgs const &args)
    {
        std::string kind = std::to_string(int(args.Kind()));
        if (link_) {
            xbrt::run_log::shell("activation kind " + kind + " ignored: this process already has its link");
            return;
        }
        xbrt::run_log::shell("activation kind " + kind);
        if (args.Kind() != ActivationKind::Protocol && read_launch_txt() == "m0-probe") {
            g_m0 = true;
            link_ = Link{std::nullopt, "link m0-probe (launch.txt)", LineColour::White};
            start_boot();
            return;
        }
        link_ = resolve_link(args);
        start_boot();
    }

    void start_boot()
    {
        if (!link_ || !gfx_tried_ || boot_started_) return;
        boot_started_ = true;
        g_boot_running = true;
        std::thread([link = *link_] {
            try {
                boot(link);
            } catch (std::exception const &e) {
                report("shell", std::string("shell FAIL boot thread: ") + e.what(), LineColour::Red);
                xbrt::run_log::run("END");
                g_boot_running = false;
            } catch (hresult_error const &e) {
                report("shell", "shell FAIL boot thread: " + to_string(e.message()), LineColour::Red);
                xbrt::run_log::run("END");
                g_boot_running = false;
            }
        }).detach();
    }

    void return_to_hub(std::string_view why)
    {
        if (left_) return;
        left_ = true;
        std::optional<Failure> failure;
        {
            std::lock_guard<std::mutex> g(g_failure_lock);
            failure = g_first_failure;
        }
        xbrt::run_log::shell("return: " + std::string(why));
        if (!link_ || !link_->request || link_->request->return_uri.empty()) return exit_now();
        xbrt::ReturnFailure named = failure ? xbrt::return_failure(failure->kind, failure->line) : xbrt::ReturnFailure{};
        std::u16string uri =
            xbrt::return_link(link_->request->return_uri, failure ? "error" : "ok", named.kind, named.missing);
        xbrt::run_log::shell("return link " + utf8(uri));
        try {
            // Exit only once the launcher has taken the link: exiting first can drop it.
            Windows::System::Launcher::LaunchUriAsync(Uri(hstring(std::wstring_view(
                reinterpret_cast<const wchar_t *>(uri.data()), uri.size()))))
                .Completed([this](auto &&, auto &&) { exit_now(); });
        } catch (hresult_error const &e) {
            xbrt::run_log::shell("return link not launched: " + to_string(e.message()));
            exit_now();
        }
    }

    void exit_now()
    {
        try {
            CoreApplication::Exit();
        } catch (hresult_error const &e) {
            xbrt::run_log::shell("CoreApplication::Exit: " + to_string(e.message()));
        }
    }

    void Run()
    {
        window_.Activate();
        xbrt::gfx::DisplayMode mode = xbrt::gfx::display_mode();
        std::string log;
        xbrt::gfx::create(static_cast<::IUnknown *>(get_abi(window_)), mode, log);
        xbrt::run_log::shell(log);
        gfx_tried_ = true;
        start_boot();

        std::vector<uint8_t> frame;
        uint64_t shown = ~uint64_t(0);
        ULONGLONG polled = 0, chord_since = 0;
        bool b_was_down = false;
        CoreDispatcher dispatcher = window_.Dispatcher();
        while (!closed_ && !left_) {
            dispatcher.ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
            ULONGLONG now = GetTickCount64();
            if (now - polled >= 100) {
                polled = now;
                // While the title runs its buttons are the title's; holding View and Menu for a second leaves.
                if (g_boot_running) {
                    bool chord = held(GamepadButtons::View | GamepadButtons::Menu);
                    chord_since = chord ? (chord_since ? chord_since : now) : 0;
                    if (chord_since && now - chord_since >= 1000) return_to_hub("View and Menu");
                } else {
                    bool down = held(GamepadButtons::B);
                    if (down && !b_was_down) return_to_hub("B");
                    b_was_down = down;
                }
                if (g_devices_attached) g_screen.replace("pad 1:", xbrt::pad_line(), LineColour::Grey);
                if (g_guest_left) return_to_hub("guest exit");
            }
            uint64_t version = g_screen.version();
            bool changed = version != shown;
            if (changed) {
                g_screen.render(mode.width, mode.height, frame);
                shown = version;
            }
            xbrt::gfx::present(frame.data(), changed);
        }
        return_to_hub("window closed");
    }
};

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    if (auto m = xbrt::GuestMemory::reserve(xbrt::RamSize::Retail64))
        g_memory = std::move(*m);
    else
        g_reserve_error = m.error();
    init_apartment();
    CoreApplication::Run(make<App>());
    if (g_boot_running) TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}
