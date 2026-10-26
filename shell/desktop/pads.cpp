// The desktop host's pad for the fork's input reader: a scripted controller in slot 0 that holds A with the left stick
// right for half a second of every two, counted from the first read, so lab/pad sees a press and a release whenever
// its USB stack is up. The CI runner has no pad of its own.
#include <windows.h>
#include <xinput.h>

namespace {

constexpr ULONGLONG kPeriodMs = 2000, kPressFromMs = 1000, kPressToMs = 1500;
ULONGLONG g_first_read;

}  // namespace

extern "C" int xbox_shell_pad_read(unsigned slot, XINPUT_GAMEPAD *out)
{
    if (slot != 0) return 0;
    ULONGLONG now = GetTickCount64();
    if (!g_first_read) g_first_read = now;
    ULONGLONG phase = (now - g_first_read) % kPeriodMs;
    *out = {};
    if (phase >= kPressFromMs && phase < kPressToMs) {
        out->wButtons = XINPUT_GAMEPAD_A;
        out->sThumbLX = 32767;
    }
    return 1;
}

extern "C" void xbox_shell_pad_rumble(unsigned, unsigned short, unsigned short) {}
