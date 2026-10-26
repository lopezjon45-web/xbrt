#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace xbrt {

enum class LineColour : uint8_t { White, Green, Red, Grey };

class StatusScreen {
public:
    void add(std::string_view text, LineColour colour);
    // Replaces the last line that starts with `prefix` (a stage reporting progress), or adds one.
    void replace(std::string_view prefix, std::string_view text, LineColour colour);
    uint64_t version() const;   // bumps on every change, so the presenter re-renders only when needed

    // Draws every line, scaled so 64 columns and the margins fit, into `rgba` (width * height * 4 bytes, row-major).
    void render(uint32_t width, uint32_t height, std::vector<uint8_t> &rgba) const;

private:
    struct Line {
        std::string text;
        LineColour colour;
    };
    mutable std::mutex lock_;
    std::vector<Line> lines_;
    uint64_t version_ = 0;
};

}  // namespace xbrt
