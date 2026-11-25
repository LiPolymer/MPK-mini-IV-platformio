// SPDX-License-Identifier: MIT
#pragma once

#include <stdint.h>

// Four contact-time breakpoints (20 us each), followed by the black-key scale in tenths.
static const uint16_t mpk4_key_curve_defaults[3][5] = {
    {6400, 2000, 800, 140, 9}, {4800, 1000, 400, 140, 9}, {3200, 800, 300, 100, 9},
};
