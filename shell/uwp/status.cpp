#include "shell/uwp/status.h"

#include "shell/uwp/font8x8.h"

#include <algorithm>
#include <cstring>

namespace xbrt {

namespace {

constexpr uint32_t kColumns = 64;
constexpr uint32_t kMargin = 2;

struct Rgb {
    uint8_t r, g, b;
};

Rgb colour_of(LineColour c)
{
    switch (c) {
    case LineColour::White: return {230, 230, 230};
    case LineColour::Green: return {80, 220, 100};
    case LineColour::Red: return {240, 70, 60};
    case LineColour::Grey: return {140, 140, 140};
    }
    return {230, 230, 230};
}

}  // namespace

void StatusScreen::add(std::string_view text, LineColour colour)
{
    std::lock_guard<std::mutex> g(lock_);
    lines_.push_back({std::string(text), colour});
    version_++;
}

void StatusScreen::replace(std::string_view prefix, std::string_view text, LineColour colour)
{
    std::lock_guard<std::mutex> g(lock_);
    for (auto it = lines_.rbegin(); it != lines_.rend(); ++it) {
        if (it->text.starts_with(prefix)) {
            *it = {std::string(text), colour};
            version_++;
            return;
        }
    }
    lines_.push_back({std::string(text), colour});
    version_++;
}

uint64_t StatusScreen::version() const
{
    std::lock_guard<std::mutex> g(lock_);
    return version_;
}

void StatusScreen::render(uint32_t width, uint32_t height, std::vector<uint8_t> &rgba) const
{
    rgba.assign(size_t(width) * height * 4, 0);
    for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
    uint32_t scale = std::max(1u, width / (8 * (kColumns + 2 * kMargin)));
    uint32_t cell = 8 * scale;
    uint32_t rows = height / cell;
    if (rows <= 2 * kMargin) return;
    uint32_t visible = rows - 2 * kMargin;

    std::lock_guard<std::mutex> g(lock_);
    size_t first = lines_.size() > visible ? lines_.size() - visible : 0;
    for (size_t li = first; li < lines_.size(); li++) {
        const Line &line = lines_[li];
        Rgb c = colour_of(line.colour);
        uint32_t y0 = (kMargin + uint32_t(li - first)) * cell;
        for (size_t ci = 0; ci < line.text.size() && ci < kColumns; ci++) {
            unsigned char ch = line.text[ci];
            if (ch >= 128) ch = '?';
            uint32_t x0 = (kMargin + uint32_t(ci)) * cell;
            for (uint32_t r = 0; r < 8; r++) {
                uint8_t bits = kFont8x8[ch][r];
                if (!bits) continue;
                for (uint32_t b = 0; b < 8; b++) {
                    if (!(bits & (1u << b))) continue;
                    for (uint32_t sy = 0; sy < scale; sy++) {
                        uint32_t y = y0 + r * scale + sy;
                        if (y >= height) break;
                        uint8_t *row = rgba.data() + (size_t(y) * width + x0 + b * scale) * 4;
                        for (uint32_t sx = 0; sx < scale && x0 + b * scale + sx < width; sx++) {
                            row[sx * 4] = c.r;
                            row[sx * 4 + 1] = c.g;
                            row[sx * 4 + 2] = c.b;
                        }
                    }
                }
            }
        }
    }
}

}  // namespace xbrt
