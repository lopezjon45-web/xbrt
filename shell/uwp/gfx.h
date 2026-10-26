#pragma once

#include <cstdint>
#include <string>
#include <unknwn.h>

namespace xbrt::gfx {

struct DisplayMode {
    unsigned width, height, hz;
    bool from_hdmi;
};

// The HDMI mode clamped to 1920x1080, or 1280x720 at 60 Hz when HdmiDisplayInformation does not answer. UI thread.
DisplayMode display_mode();

// UI thread, once. Every step's HRESULT goes to `log`; false when any step failed or the level is below 11_1.
bool create(IUnknown *core_window, const DisplayMode &mode, std::string &log);

// The feature-level line for the status screen ("device 11_1" or the failing step).
std::string device_line();

// Uploads `rgba` (width * height * 4 of the mode's size) when `changed`, copies it into the current back buffer and
// presents with vsync. UI thread.
void present(const uint8_t *rgba, bool changed);

}  // namespace xbrt::gfx
