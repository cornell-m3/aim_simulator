#pragma once

#include <cassert>
#include <cstdint>

namespace Ramulator::aim_channel_mask {

// Width of an AiM channel mask. Channel indices are in [0, kWidth).
inline constexpr int kWidth = 32;

// Count the number of channels selected by ch_mask.
// Precondition: ch_mask > 0.
inline uint8_t count(int64_t ch_mask) {
    assert(ch_mask > 0);
    uint8_t n = 0;
    for (int i = 0; i < kWidth; i++) {
        if (ch_mask & (1LL << i)) n++;
    }
    return n;
}

// Pop the lowest set bit of ch_mask (in-place) and return its index in [0, kWidth).
// Precondition: at least one bit in [0, kWidth) is set.
inline uint8_t pop_lowest(int64_t &ch_mask) {
    uint64_t u = static_cast<uint64_t>(ch_mask);
    assert((u & 0xffffffffULL) != 0);
    for (int i = 0; i < kWidth; i++) {
        if (u & (1ULL << i)) {
            u &= ~(1ULL << i);
            ch_mask = static_cast<int64_t>(u);
            return i;
        }
    }
    assert(false);
    return 0;
}

} // namespace Ramulator::aim_channel_mask
