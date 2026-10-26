// The only XDK functions the runtime replaces. Keyed by XbSymbolDatabase (library, name), so one row covers every
// title whose library the database knows.
#pragma once

#include <cstdint>
#include <iterator>
#include <string_view>

namespace xbrt {

enum class HleId : uint16_t {
    QueryPerformanceCounter,
    XLaunchNewImageA,
    XapiBootToDash,
    OutputDebugStringA,
    Count_
};

struct HleRow {
    std::string_view library;
    std::string_view symbol;
    HleId id;
    uint8_t stdcall_arg_bytes;
};

inline constexpr HleRow kHleTable[] = {
    {"XAPILIB", "QueryPerformanceCounter", HleId::QueryPerformanceCounter, 4},
    {"XAPILIB", "XLaunchNewImageA", HleId::XLaunchNewImageA, 8},
    {"XAPILIB", "XapiBootToDash", HleId::XapiBootToDash, 12},
    {"XAPILIB", "OutputDebugStringA", HleId::OutputDebugStringA, 4},
};
static_assert(std::size(kHleTable) == static_cast<size_t>(HleId::Count_), "one row per HleId");

}  // namespace xbrt
