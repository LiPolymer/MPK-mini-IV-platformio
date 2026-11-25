// SPDX-License-Identifier: MIT
#pragma once

#include "mpk4/usb_midi.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace mpk4 {

enum class Button : std::uint8_t {
    octaveDown = 0, octaveUp, arp, latch, noteRepeat, tap,
    bank = 8, minus, plus, shift, daw, undo, encoder,
    loop = 16, play, record, overdub
};

enum class Wheel : std::uint8_t { pitch, modulation };

enum class InputKind : std::uint8_t {
    matrix, keyOn, keyOff, buttonDown, buttonUp, knob, encoder,
    padOn, padOff, wheel, pedal, adc, padPressure
};

// See the event table in README.md for the meaning of each field.
struct InputEvent {
    InputKind kind;
    std::uint8_t index;
    std::uint8_t value;
    std::int16_t delta;
    std::uint16_t sample;
};

struct WheelCalibration {
    std::uint16_t minimum;
    std::uint16_t centre;
    std::uint16_t maximum;
};

struct KnobWipers { std::uint16_t a, b; };

class Board {
public:
    // Returns false if the crystal did not start. The controls and DIN still run.
    static bool begin();
    static void poll();
    static void enableWatchdog();
    static std::uint32_t millis();
    static std::uint32_t ticks();  // One tick = 20 us. Use unsigned differences.
    static std::uint32_t resetFlags();
    static std::array<char, 17> serial();
    [[noreturn]] static void rebootToBootloader();
};

class Controls {
public:
    static constexpr unsigned keyCount = 25;
    static constexpr unsigned padCount = 8;
    static constexpr unsigned knobCount = 8;
    static constexpr unsigned matrixLineCount = 11;
    static constexpr unsigned adcChannelCount = 6;
    static constexpr unsigned muxPositionCount = 8;

    static bool read(InputEvent& event);
    static void enableRawEvents(bool enabled);
    static std::uint32_t idleMillis();
    static std::uint32_t droppedEvents();
    static std::uint32_t droppedMatrixEvents();
    static bool keyDown(unsigned key);
    static bool padDown(unsigned pad);
    static bool buttonDown(Button button);
    static std::uint8_t matrixBits(unsigned line);
    static std::uint16_t adc(unsigned channel, unsigned mux);
    static std::uint16_t rawPad(unsigned pad);
    static KnobWipers knobWipers(unsigned knob);
    static std::uint16_t rawWheel(Wheel wheel);
    static WheelCalibration wheelCalibration(Wheel wheel);
    static std::int32_t encoderPosition();
    static bool pedalPinHigh();
    static std::array<std::uint16_t, 5> keyCurve(unsigned curve);
    static void setKeyCurve(unsigned curve);
};

class Leds {
public:
    static constexpr std::uint8_t maxBrightness = 32;
    static bool pad(unsigned index, std::uint8_t r, std::uint8_t g, std::uint8_t b);
    static bool button(Button button, std::uint8_t brightness, bool secondColour = false);
    static bool beatButton(Button button, std::uint8_t brightness, std::uint8_t flash,
                           bool secondColour = false);
};

using Rgb565 = std::uint16_t;

constexpr Rgb565 rgb565(std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    return static_cast<Rgb565>(((r & 0xf8u) << 8) | ((g & 0xfcu) << 3) | (b >> 3));
}

class Display {
public:
    static constexpr int width = 160;
    static constexpr int height = 80;

    class Canvas {
    public:
        Canvas(const Canvas&) = delete;
        Canvas& operator=(const Canvas&) = delete;

        void clear(Rgb565 colour);
        void pixel(int x, int y, Rgb565 colour);
        void fillRect(int x, int y, int w, int h, Rgb565 colour);

    private:
        friend class Display;
        explicit Canvas(std::uint16_t* pixels) : pixels_(pixels) {}
        std::uint16_t* pixels_;
    };

    static bool busy();

    // Raw RGB565 words in panel transfer byte order. The caller keeps the frame stable.
    static const std::uint16_t* framebuffer();

    // The canvas is valid for the callback only. False means the DMA is busy.
    template<class Draw>
    static bool draw(Draw&& render)
    {
        auto* pixels = acquire();
        if (!pixels) return false;
        Canvas canvas(pixels);
        render(canvas);
        present();
        return true;
    }

private:
    static std::uint16_t* acquire();
    static void present();
};

class UsbMidi {
public:
    static constexpr unsigned hostToDeviceCables = 5;
    static constexpr unsigned deviceToHostCables = 4;
    static bool ready();
    static bool read(UsbMidiPacket& packet);
    static bool write(const UsbMidiPacket& packet);
};

class DinMidi {
public:
    // Queues the complete byte sequence, or returns false without queuing any of it.
    static bool write(const std::uint8_t* bytes, std::size_t size);
    static bool realtime(std::uint8_t byte);
    static std::uint32_t queued();
};

class MidiClock {
public:
    // Start on the next 20-us tick. Clock is 24 PPQN, with a range of 20 to 300 BPM.
    static bool start(unsigned bpm, bool forwardToDin);
    static bool setTempo(unsigned bpm, bool forwardToDin);
    static void stop();
    static bool running();
    static std::uint32_t emitted();
};

} // namespace mpk4
