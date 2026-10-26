/*
 * Versal MMCME5 output-divider (O) encoding, shared by the clock driver and
 * its unit tests.
 *
 * With PREDIV2 set, the realized divider is 2 * (HIGH + LOW + EDGE) + P5EN,
 * where EDGE is bit 8 and P5EN is bit 13 of the leaf's first register. The
 * encoder below writes EDGE = (O % 4 > 1) and P5EN = O % 2, which realizes O
 * exactly for every O. Xilinx's clk_wiz_v1_10 decoder reads bit 8 as P5EN and
 * bit 15 (P5FEDGE) as EDGE instead; that decode reports O - 1 for O % 4 == 2
 * and O + 1 for O % 4 == 1 (e.g. 107.16 MHz for a 103.59 MHz clock), so a
 * correctly programmed clock read back as overclocked.
 */
#ifndef VRTD_CLOCK_MATH_H
#define VRTD_CLOCK_MATH_H

#include <stdint.h>

#define XCLK_LEAF_EDGE      (1u << 8)
#define XCLK_LEAF_MX        (1u << 9)
#define XCLK_LEAF_PREDIV2   (1u << 11)
#define XCLK_LEAF_USED      (1u << 12)
#define XCLK_LEAF_P5EN      (1u << 13)
#define XCLK_LEAF_P5FEDGE   (1u << 15)
#define XCLK_LEAF_LOW_MASK  0x00FFu
#define XCLK_LEAF_HIGH_MASK 0xFF00u

struct xclk_leaf_regs {
    uint32_t ctrl;   /* first register of the leaf pair */
    uint32_t counts; /* second register: HIGH << 8 | LOW */
};

static inline struct xclk_leaf_regs xclk_encode_o(uint32_t o)
{
    uint32_t high_time = o / 4u;
    struct xclk_leaf_regs r;
    r.ctrl = XCLK_LEAF_PREDIV2 | XCLK_LEAF_USED | XCLK_LEAF_MX;
    if ((o % 4u) > 1u) {
        r.ctrl |= XCLK_LEAF_EDGE;
    }
    if ((o % 2u) != 0u) {
        r.ctrl |= XCLK_LEAF_P5EN | XCLK_LEAF_P5FEDGE;
    }
    r.counts = high_time | (high_time << 8u);
    return r;
}

static inline uint32_t xclk_decode_divo(uint32_t ctrl, uint32_t counts)
{
    uint32_t low = counts & XCLK_LEAF_LOW_MASK;
    uint32_t high = (counts & XCLK_LEAF_HIGH_MASK) >> 8u;
    uint32_t edge = (ctrl & XCLK_LEAF_EDGE) ? 1u : 0u;
    uint32_t p5en = (ctrl & XCLK_LEAF_P5EN) ? 1u : 0u;
    uint32_t prediv = (ctrl & XCLK_LEAF_PREDIV2) ? 1u : 0u;
    uint32_t divo = (prediv + 1u) * (high + low + edge) + prediv * p5en;
    return divo ? divo : 1u;
}

#endif
