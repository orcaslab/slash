#include <gtest/gtest.h>

#include <cstdint>

extern "C" {
#include "clock.h"
#include "clock_math.h"
}

namespace {

constexpr uint32_t kPrimInHz = 100000000u;

TEST(ClockMath, EncodedDividerIsRealizedExactly)
{
    for (uint32_t o = 2; o <= 511; ++o) {
        xclk_leaf_regs r = xclk_encode_o(o);
        EXPECT_EQ(xclk_decode_divo(r.ctrl, r.counts), o) << "o=" << o;
        EXPECT_EQ(clock_driver_realized_divo(o), o) << "o=" << o;
    }
}

// The 2026-10-25 case: m=404 d=13 o=30 for a 103.59 MHz image read back as
// 107.16 MHz (divider 29) with the old decode.
TEST(ClockMath, FiresimMidCaseReadsBackBelowRequest)
{
    const uint64_t fvco = uint64_t{kPrimInHz} * 404u / 13u;
    xclk_leaf_regs r = xclk_encode_o(30);
    EXPECT_EQ(xclk_decode_divo(r.ctrl, r.counts), 30u);
    EXPECT_LE(fvco / 30u, 103594737u);
}

TEST(ClockPlan, NeverAboveRequestAcrossTheRange)
{
    for (uint32_t target = 50000000u; target <= 400000000u; target += 250000u) {
        uint32_t m = 0, d = 0, o = 0;
        uint64_t rate = 0;
        ASSERT_EQ(clock_driver_plan_rate_hz(kPrimInHz, target, &m, &d, &o, &rate), 0) << target;
        EXPECT_LE(rate, target) << "target=" << target << " m=" << m << " d=" << d << " o=" << o;
        EXPECT_GE(rate, target - target / 50) << "more than 2% below target=" << target;
        const uint64_t fvco = uint64_t{kPrimInHz} * m / d;
        EXPECT_GE(fvco, 2160000000ull);
        EXPECT_LE(fvco, 4320000000ull);
        EXPECT_EQ(rate, fvco / o);
    }
}

TEST(ClockPlan, SafeRateIsBelowEveryImage)
{
    uint32_t m = 0, d = 0, o = 0;
    uint64_t rate = 0;
    ASSERT_EQ(clock_driver_plan_rate_hz(kPrimInHz, CLOCK_DRIVER_SAFE_USER_RATE_HZ, &m, &d, &o, &rate), 0);
    EXPECT_LE(rate, CLOCK_DRIVER_SAFE_USER_RATE_HZ);
    EXPECT_LT(CLOCK_DRIVER_SAFE_USER_RATE_HZ, 100000000u);
}

} // namespace
