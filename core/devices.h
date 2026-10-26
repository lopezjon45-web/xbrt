// The original Xbox's device apertures, as every title addresses them. A guest access to one with no model attached is
// a DeviceRegister refusal that names the device.
#pragma once

#include <cstdint>
#include <string_view>

namespace xbrt {

struct DeviceAperture {
    uint32_t base, size;
    std::string_view name;
};

inline constexpr DeviceAperture kDeviceApertures[] = {
    {0xFD000000, 0x01000000, "NV2A"},
    {0xFE800000, 0x00080000, "APU"},
    {0xFEC00000, 0x00001000, "AC97"},
    {0xFED00000, 0x00001000, "USB0 OHCI"},
    {0xFED08000, 0x00001000, "USB1 OHCI"},
    {0xFEF00000, 0x00000400, "NIC"},
};

constexpr const DeviceAperture *device_aperture_at(uint32_t addr)
{
    for (const DeviceAperture &d : kDeviceApertures)
        if (addr - d.base < d.size) return &d;
    return nullptr;
}

}  // namespace xbrt
