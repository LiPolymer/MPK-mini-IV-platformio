// SPDX-License-Identifier: MIT
#pragma once

#include <stdint.h>

// v1.43 at 0x0801A800. With the factory coefficients at unity, use
// the remaining distance to the knee to preserve its integer rounding.
static inline uint8_t stock_pad_velocity(uint16_t peak)
{
    const int32_t threshold = 209;
    const int32_t knee = 975;
    const int32_t slope = knee * 65536 / (knee - threshold);
    const int32_t remaining = knee - (peak > threshold ? peak : threshold);
    const int32_t velocity = (knee - (remaining * slope) / 65536) / 8;
    return (uint8_t)(velocity < 1 ? 1 : velocity > 127 ? 127 : velocity);
}
