// The pads for the fork's input reader (xbox/xbox_shell.h): Windows.Gaming.Input gamepads in XInput's shape. Each pad
// keeps the slot it took until it is unplugged; the list is rescanned at most once a second. Called only from the
// reader's own thread.
#include <windows.h>
#include <xinput.h>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

using winrt::Windows::Gaming::Input::Gamepad;
using winrt::Windows::Gaming::Input::GamepadButtons;
using winrt::Windows::Gaming::Input::GamepadReading;
using winrt::Windows::Gaming::Input::GamepadVibration;

namespace {

std::array<std::optional<Gamepad>, 4> g_slots;
ULONGLONG g_scanned;

void rescan()
{
    ULONGLONG now = GetTickCount64();
    if (now - g_scanned < 1000 && g_scanned) return;
    g_scanned = now;
    auto present = Gamepad::Gamepads();
    for (std::optional<Gamepad> &slot : g_slots) {
        if (!slot) continue;
        uint32_t index;
        if (!present.IndexOf(*slot, index)) slot.reset();
    }
    for (Gamepad const &pad : present) {
        if (std::any_of(g_slots.begin(), g_slots.end(), [&](const std::optional<Gamepad> &s) { return s && *s == pad; }))
            continue;
        auto free = std::find_if(g_slots.begin(), g_slots.end(), [](const std::optional<Gamepad> &s) { return !s; });
        if (free == g_slots.end()) break;
        *free = pad;
    }
}

SHORT axis(double v) { return SHORT(std::clamp(std::lround(v * 32767.0), -32768L, 32767L)); }

BYTE trigger(double v) { return BYTE(std::clamp(std::lround(v * 255.0), 0L, 255L)); }

}  // namespace

extern "C" int xbox_shell_pad_read(unsigned slot, XINPUT_GAMEPAD *out)
{
    if (slot >= g_slots.size()) return 0;
    try {
        rescan();
        if (!g_slots[slot]) return 0;
        GamepadReading r = g_slots[slot]->GetCurrentReading();
        static constexpr std::pair<GamepadButtons, WORD> kButtons[] = {
            {GamepadButtons::A, XINPUT_GAMEPAD_A},
            {GamepadButtons::B, XINPUT_GAMEPAD_B},
            {GamepadButtons::X, XINPUT_GAMEPAD_X},
            {GamepadButtons::Y, XINPUT_GAMEPAD_Y},
            {GamepadButtons::View, XINPUT_GAMEPAD_BACK},
            {GamepadButtons::Menu, XINPUT_GAMEPAD_START},
            {GamepadButtons::LeftThumbstick, XINPUT_GAMEPAD_LEFT_THUMB},
            {GamepadButtons::RightThumbstick, XINPUT_GAMEPAD_RIGHT_THUMB},
            {GamepadButtons::LeftShoulder, XINPUT_GAMEPAD_LEFT_SHOULDER},
            {GamepadButtons::RightShoulder, XINPUT_GAMEPAD_RIGHT_SHOULDER},
            {GamepadButtons::DPadUp, XINPUT_GAMEPAD_DPAD_UP},
            {GamepadButtons::DPadDown, XINPUT_GAMEPAD_DPAD_DOWN},
            {GamepadButtons::DPadLeft, XINPUT_GAMEPAD_DPAD_LEFT},
            {GamepadButtons::DPadRight, XINPUT_GAMEPAD_DPAD_RIGHT},
        };
        *out = {};
        for (const auto &[from, to] : kButtons)
            if ((r.Buttons & from) != GamepadButtons::None) out->wButtons |= to;
        out->bLeftTrigger = trigger(r.LeftTrigger);
        out->bRightTrigger = trigger(r.RightTrigger);
        out->sThumbLX = axis(r.LeftThumbstickX);
        out->sThumbLY = axis(r.LeftThumbstickY);
        out->sThumbRX = axis(r.RightThumbstickX);
        out->sThumbRY = axis(r.RightThumbstickY);
        return 1;
    } catch (winrt::hresult_error const &) {
        g_slots[slot].reset();
        return 0;
    }
}

extern "C" void xbox_shell_pad_rumble(unsigned slot, unsigned short low, unsigned short high)
{
    if (slot >= g_slots.size() || !g_slots[slot]) return;
    try {
        GamepadVibration v{};
        v.LeftMotor = low / 65535.0;
        v.RightMotor = high / 65535.0;
        g_slots[slot]->Vibration(v);
    } catch (winrt::hresult_error const &) {
    }
}
