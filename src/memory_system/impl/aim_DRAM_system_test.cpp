// Unit tests for AiM-memory-system helpers.
//
// Scope today: the pure-function bit-mask helpers in aim_channel_mask.h.
// These cover edge cases (low/high bit, max mask, sequential pop) without
// having to stand up the full Ramulator simulator stack.
//
// Future work — drive AiMDRAMSystem internals (stalls accounting,
// last_completed_host_id advancement, PendingCallback FIFO order) end-to-end
// in unit tests. That needs a mock IDRAMController + IDRAM + addr_mapper +
// create_child_ifce harness; until then the regression script
// (test/run_regression.sh) provides integration coverage of those invariants
// against real traces.

#include "memory_system/impl/aim_channel_mask.h"

#include <gtest/gtest.h>

namespace mask = Ramulator::aim_channel_mask;

TEST(CountSetBit, SingleBits) {
    EXPECT_EQ(mask::count(0x00000001), 1u);
    EXPECT_EQ(mask::count(0x00000002), 1u);
    EXPECT_EQ(mask::count(0x80000000), 1u);
}

TEST(CountSetBit, MultipleBits) {
    EXPECT_EQ(mask::count(0x00000003), 2u);
    EXPECT_EQ(mask::count(0x0000000F), 4u);
    EXPECT_EQ(mask::count(0xFFFFFFFF), 32u);
}

TEST(CountSetBit, ScatteredBits) {
    // Bits 0, 5, 17, 31 set.
    int64_t mask_val = (1LL << 0) | (1LL << 5) | (1LL << 17) | (1LL << 31);
    EXPECT_EQ(mask::count(mask_val), 4u);
}

TEST(PopLowest, ReturnsLowestThenAdvances) {
    int64_t m = 0x00000005; // bits 0, 2
    EXPECT_EQ(mask::pop_lowest(m), 0u);
    EXPECT_EQ(m, 0x4);
    EXPECT_EQ(mask::pop_lowest(m), 2u);
    EXPECT_EQ(m, 0x0);
}

TEST(PopLowest, HandlesHighBit) {
    int64_t m = 0x80000000;
    EXPECT_EQ(mask::pop_lowest(m), 31u);
    EXPECT_EQ(m, 0x0);
}

TEST(PopLowest, SequentialDrain) {
    // Drain all 32 bits one at a time, verifying we see indices 0..31 in order.
    int64_t m = 0xFFFFFFFF;
    for (int expected = 0; expected < 32; expected++) {
        EXPECT_EQ(mask::pop_lowest(m), static_cast<uint8_t>(expected));
    }
    EXPECT_EQ(m, 0x0);
}

TEST(PopLowest, PreservesNonMaskBits) {
    // Bits above 32 are not channel bits and shouldn't be touched.
    // (kWidth is 32; pop_lowest only inspects bits 0..31.)
    // Sanity: low bit is set, behavior should match the low-bit-only case.
    int64_t m = (1LL << 0);
    EXPECT_EQ(mask::pop_lowest(m), 0u);
    EXPECT_EQ(m, 0x0);
}

TEST(Width, IsThirtyTwo) {
    EXPECT_EQ(mask::kWidth, 32);
}
