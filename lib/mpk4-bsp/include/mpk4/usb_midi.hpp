// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstdint>

namespace mpk4 {

// USB-MIDI 1.0 event packet: cable/CIN, then up to three MIDI bytes.
using UsbMidiPacket = std::array<std::uint8_t, 4>;

namespace usb_midi {

constexpr unsigned payloadSize(std::uint8_t header)
{
    constexpr std::uint8_t sizes[] = {0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1};
    return sizes[header & 15];
}

constexpr UsbMidiPacket packet(unsigned cable, unsigned cin, std::uint8_t first = 0,
                               std::uint8_t second = 0, std::uint8_t third = 0)
{
    return {static_cast<std::uint8_t>((cable << 4) | cin), first, second, third};
}

// Initialize a packet for the next bytes of a SysEx stream. remaining includes the end byte.
constexpr UsbMidiPacket sysexFragment(unsigned cable, unsigned remaining)
{
    return packet(cable, remaining > 3 ? 4 : 4 + remaining);
}

} // namespace usb_midi
} // namespace mpk4
