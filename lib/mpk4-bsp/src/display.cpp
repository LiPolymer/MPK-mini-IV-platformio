// SPDX-License-Identifier: MIT
#include "mpk4/bsp.hpp"
#include <algorithm>

namespace mpk4 {
namespace {
constexpr std::uint16_t panelWord(Rgb565 colour)
{
    // SPI reads the little-endian SRAM buffer as bytes, while the panel expects MSB first.
    return static_cast<std::uint16_t>((colour << 8) | (colour >> 8));
}
}

void Display::Canvas::clear(Rgb565 colour)
{
    std::fill_n(pixels_, width * height, panelWord(colour));
}

void Display::Canvas::pixel(int x, int y, Rgb565 colour)
{
    if (x >= 0 && x < width && y >= 0 && y < height) {
        pixels_[y * width + x] = panelWord(colour);
    }
}

void Display::Canvas::fillRect(int x, int y, int w, int h, Rgb565 colour)
{
    if (w <= 0 || h <= 0 || x >= width || y >= height) return;
    const auto right = static_cast<int>(std::min<std::int64_t>(width, std::int64_t{x} + w));
    const auto bottom = static_cast<int>(std::min<std::int64_t>(height, std::int64_t{y} + h));
    const auto left = std::max(x, 0);
    if (right <= left) return;
    for (int row = std::max(y, 0); row < bottom; ++row) {
        std::fill_n(pixels_ + row * width + left, right - left, panelWord(colour));
    }
}
}
