// SPDX-License-Identifier: MIT
#include "mpk4/bsp.hpp"

extern "C" {
#include "at32f402_405.h"
#include "board.h"
#include "inputs.h"
void usb_init(void);
}
#include "tusb.h"

namespace mpk4 {
namespace {
bool initialized;
bool crystalReady;
bool watchdogEnabled;
bool drawing;
std::uint8_t updateMatch;

static_assert(Controls::keyCount == N_KEYS && Controls::padCount == N_PADS &&
              Controls::knobCount == N_KNOBS && Controls::matrixLineCount == MX_LINES &&
              Controls::adcChannelCount == ADC_CH && Controls::muxPositionCount == MUX_N);
static_assert(Leds::maxBrightness == LED_MAX);

constexpr unsigned number(Button button) { return static_cast<unsigned>(button); }
constexpr unsigned number(Wheel wheel) { return static_cast<unsigned>(wheel); }

std::uint8_t buttonLed(Button button, bool second)
{
    const auto index = number(button);
    return index < N_BUTTONS ? BTN_LEDS[index][second] : NO_LED;
}

constexpr std::array kinds{
    InputKind::matrix, InputKind::keyOn, InputKind::keyOff,
    InputKind::buttonDown, InputKind::buttonUp, InputKind::knob,
    InputKind::encoder, InputKind::padOn, InputKind::padOff,
    InputKind::wheel, InputKind::pedal, InputKind::adc, InputKind::padPressure
};
static_assert(EV_MATRIX == 0 && EV_KEY_ON == 1 && EV_KEY_OFF == 2 &&
              EV_BTN_DOWN == 3 && EV_BTN_UP == 4 && EV_KNOB == 5 && EV_ENC == 6 &&
              EV_PAD_ON == 7 && EV_PAD_OFF == 8 && EV_WHEEL == 9 && EV_PEDAL == 10 &&
              EV_ADC == 11 && EV_PAD_PRESSURE == 12 && kinds.size() == EV_PAD_PRESSURE + 1);

void inspectUpdateRequest(const UsbMidiPacket& packet)
{
    // The updater sends one 8-byte SysEx on cable 0. Realtime bytes can
    // occur between SysEx packets. Other messages end a partial match.
    static constexpr UsbMidiPacket first{0x04, 0xf0, 0x47, 0x00};
    static constexpr UsbMidiPacket second{0x04, 0x5d, 0x74, 0x00};
    if (packet[0] == 0x0f && packet[1] >= 0xf8) return;
    if (updateMatch == 2 && packet[0] == 0x06 && packet[1] == 0x00 && packet[2] == 0xf7) {
        Board::rebootToBootloader();
    }
    if (updateMatch == 1 && packet == second) {
        updateMatch = 2;
    } else {
        updateMatch = packet == first ? 1 : 0;
    }
}

constexpr std::uint32_t clockPeriod(unsigned bpm)
{
    return 60u * 50000u * 256u / (24u * bpm);
}
}

bool Board::begin()
{
    if (initialized) return crystalReady;
    crystalReady = clock_init();
    board_init();
    inputs_init();
    if (crystalReady) usb_init();
    initialized = true;
    return crystalReady;
}

void Board::poll()
{
    if (!initialized) return;
    if (watchdogEnabled) wdt_feed();
    if (crystalReady) tud_task();
    inputs_poll();
}

void Board::enableWatchdog()
{
    if (!initialized || watchdogEnabled) return;
    wdt_start();
    watchdogEnabled = true;
}

std::uint32_t Board::millis() { return g_ms; }
std::uint32_t Board::ticks() { return g_ticks; }
std::uint32_t Board::resetFlags() { return g_reset_flags; }

std::array<char, 17> Board::serial()
{
    std::array<char, 17> serial{};
    unit_serial(serial.data());
    return serial;
}

[[noreturn]] void Board::rebootToBootloader() { NVIC_SystemReset(); }

bool Controls::read(InputEvent& event)
{
    input_ev_t raw;
    if (!inputs_pop(&raw)) return false;
    event = {kinds[raw.type], raw.a, raw.b, raw.d, raw.v};
    return true;
}

void Controls::enableRawEvents(bool enabled) { inputs_raw(enabled); }
std::uint32_t Controls::idleMillis() { return inputs_idle_ms(); }
std::uint32_t Controls::droppedEvents() { return inputs_dropped(); }
std::uint32_t Controls::droppedMatrixEvents() { return matrix_dropped(); }
bool Controls::keyDown(unsigned key) { return key < keyCount && key_down(static_cast<int>(key)); }
bool Controls::padDown(unsigned pad) { return pad < padCount && pad_down(static_cast<int>(pad)); }
bool Controls::buttonDown(Button button) { return button_down(static_cast<int>(button)); }

std::uint8_t Controls::matrixBits(unsigned line)
{
    return line < matrixLineCount ? matrix_state(static_cast<int>(line)) : 0;
}

std::uint16_t Controls::adc(unsigned channel, unsigned mux)
{
    if (channel >= adcChannelCount || mux >= muxPositionCount) return 0;
    return adc_raw(static_cast<int>(channel), static_cast<int>(mux));
}

std::uint16_t Controls::rawPad(unsigned pad)
{
    return pad < padCount ? pad_raw(static_cast<int>(pad)) : 0;
}

KnobWipers Controls::knobWipers(unsigned knob)
{
    if (knob >= knobCount) return {};
    const auto index = static_cast<int>(knob);
    return {knob_a(index), knob_b(index)};
}

std::uint16_t Controls::rawWheel(Wheel wheel)
{
    return number(wheel) <= WHEEL_MOD ? wheel_raw(static_cast<int>(wheel)) : 0;
}

WheelCalibration Controls::wheelCalibration(Wheel wheel)
{
    if (number(wheel) > WHEEL_MOD) return {};
    const auto& cal = g_wheel_cal[number(wheel)];
    return {cal.min, cal.centre, cal.max};
}

std::int32_t Controls::encoderPosition() { return enc_count(); }
bool Controls::pedalPinHigh() { return pedal_level(); }
std::array<std::uint16_t, 5> Controls::keyCurve(unsigned curve)
{
    std::array<std::uint16_t, 5> values{};
    inputs_get_key_curve(curve, values.data());
    return values;
}
void Controls::setKeyCurve(unsigned curve) { inputs_set_key_curve(curve); }

bool Leds::pad(unsigned index, std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    if (index >= Controls::padCount) return false;
    const auto& pins = PAD_LEDS[index];
    led_set(pins[0], r);
    led_set(pins[1], g);
    led_set(pins[2], b);
    return true;
}

bool Leds::button(Button button, std::uint8_t brightness, bool secondColour)
{
    const auto led = buttonLed(button, secondColour);
    if (led == NO_LED) return false;
    led_set(led, brightness);
    return true;
}

bool Leds::beatButton(Button button, std::uint8_t brightness, std::uint8_t flash,
                      bool secondColour)
{
    const auto led = buttonLed(button, secondColour);
    if (led == NO_LED) return false;
    led_beat(led, brightness, flash);
    return true;
}

bool UsbMidi::ready() { return initialized && crystalReady && tud_midi_mounted(); }

bool UsbMidi::read(UsbMidiPacket& packet)
{
    if (!ready()) {
        updateMatch = 0;
        return false;
    }
    if (!tud_midi_packet_read(packet.data())) return false;
    inspectUpdateRequest(packet);
    return true;
}

bool UsbMidi::write(const UsbMidiPacket& packet)
{
    const auto cin = packet[0] & 0x0f;
    return ready() && (packet[0] >> 4) < UsbMidi::deviceToHostCables && cin >= 2 &&
           tud_midi_packet_write(packet.data());
}

bool DinMidi::write(const std::uint8_t* bytes, std::size_t size)
{
    if (!initialized || !bytes || size == 0 || size > 255) return false;
    return din_write(bytes, static_cast<int>(size));
}

bool DinMidi::realtime(std::uint8_t byte)
{
    return initialized && byte >= 0xf8 && din_realtime(byte);
}

std::uint32_t DinMidi::queued() { return din_queued(); }

bool MidiClock::start(unsigned bpm, bool forwardToDin)
{
    if (!initialized || bpm < 20 || bpm > 300) return false;
    const auto period = clockPeriod(bpm);
    clk_run((Board::ticks() + 1) << 8, period, 0, forwardToDin);
    return true;
}

bool MidiClock::setTempo(unsigned bpm, bool forwardToDin)
{
    if (!clk_running() || bpm < 20 || bpm > 300) return false;
    clk_period(clockPeriod(bpm), forwardToDin);
    return true;
}

void MidiClock::stop() { clk_stop(); }
bool MidiClock::running() { return clk_running(); }
std::uint32_t MidiClock::emitted() { return clk_emitted(); }

bool Display::busy() { return !initialized || drawing || lcd_busy(); }

const std::uint16_t* Display::framebuffer() { return lcd_framebuffer(); }

std::uint16_t* Display::acquire()
{
    if (busy()) return nullptr;
    drawing = true;
    return lcd_framebuffer();
}

void Display::present()
{
    lcd_flush();
    drawing = false;
}

} // namespace mpk4
