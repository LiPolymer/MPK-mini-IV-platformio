// SPDX-License-Identifier: MIT
#include "mpk4/bsp.hpp"

#include <algorithm>
#include <array>
#include <string_view>

namespace {

constexpr int glyphWidth = 5;
constexpr int glyphHeight = 7;
constexpr int scale = 2;
constexpr int advance = (glyphWidth + 1) * scale;

struct Glyph {
    char character;
    std::array<std::uint8_t, glyphHeight> rows;
};

constexpr std::array<Glyph, 8> glyphs{{
    {'h', {0b10000, 0b10000, 0b10110, 0b11001, 0b10001, 0b10001, 0b10001}},
    {'e', {0b00000, 0b00000, 0b01110, 0b10001, 0b11111, 0b10000, 0b01110}},
    {'l', {0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}},
    {'o', {0b00000, 0b00000, 0b01110, 0b10001, 0b10001, 0b10001, 0b01110}},
    {'w', {0b00000, 0b00000, 0b10001, 0b10001, 0b10101, 0b10101, 0b01010}},
    {'r', {0b00000, 0b00000, 0b10110, 0b11001, 0b10000, 0b10000, 0b10000}},
    {'d', {0b00001, 0b00001, 0b01101, 0b10011, 0b10001, 0b10001, 0b01111}},
    {'!', {0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00000, 0b00100}},
}};

void drawGreeting(mpk4::Display::Canvas& canvas, std::string_view greeting)
{
    const int textWidth = static_cast<int>(greeting.size()) * advance - scale;
    canvas.clear(mpk4::rgb565(0, 0, 0));
    constexpr auto foreground = mpk4::rgb565(255, 255, 255);
    constexpr int top = (mpk4::Display::height - glyphHeight * scale) / 2;
    int left = (mpk4::Display::width - textWidth) / 2;

    for (char character : greeting) {
        const auto glyph = std::find_if(glyphs.begin(), glyphs.end(),
            [character](const Glyph& entry) { return entry.character == character; });
        if (glyph != glyphs.end()) {
            for (int row = 0; row < glyphHeight; ++row) {
                for (int column = 0; column < glyphWidth; ++column) {
                    if ((glyph->rows[row] >> (glyphWidth - 1 - column)) & 1) {
                        canvas.fillRect(left + column * scale, top + row * scale, scale, scale, foreground);
                    }
                }
            }
        }
        left += advance;
    }
}

} // namespace

int main()
{
    mpk4::Board::begin();
    mpk4::Board::enableWatchdog();

    bool greetingDrawn = false;
    for (;;) {
        mpk4::Board::poll();

        mpk4::UsbMidiPacket packet{};
        while (mpk4::UsbMidi::read(packet)) {
            // Handle incoming MIDI here. Reading also services the update request.
        }

        mpk4::InputEvent event{};
        while (mpk4::Controls::read(event)) {
            // Handle keys, pads and other panel controls here.
        }

        if (!greetingDrawn) {
            greetingDrawn = mpk4::Display::draw([](mpk4::Display::Canvas& canvas) {
                drawGreeting(canvas, "hello world!");
            });
        }
    }
}
