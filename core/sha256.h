#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace xbrt {

std::array<uint8_t, 32> sha256(std::span<const uint8_t> data);

}  // namespace xbrt
