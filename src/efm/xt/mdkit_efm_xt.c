/* SPDX-License-Identifier: CC0-1.0
 * mdkit_efm_xt.c - EFM-XT standalone voice
 */
#include <stdint.h>
#include "mdkit_efm_xt.h"

/* ---- Fixed-point helpers ------------------------------------------------
 *
 * 24-bit signed words with 56-bit accumulators. Products enter the high half
 * of the accumulator; rounded operations round the complete sum.
 */
#define MDKIT_MASK24 UINT32_C(0xffffff)
#define MDKIT_MASK48 UINT64_C(0xffffffffffff)
#define MDKIT_MASK56 UINT64_C(0xffffffffffffff)

static inline int64_t mdkit_signed_bits(uint64_t word, unsigned bits)
{
    const uint64_t sign = UINT64_C(1) << (bits - 1);
    const uint64_t mask = (UINT64_C(1) << bits) - 1;
    word &= mask;
    return (word & sign) ? (int64_t)word - (int64_t)(UINT64_C(1) << bits)
                         : (int64_t)word;
}

static inline int64_t mdkit_s24(uint32_t w)
{
    return mdkit_signed_bits(w & MDKIT_MASK24, 24);
}

static inline int64_t mdkit_s56(uint64_t a)
{
    return mdkit_signed_bits(a & MDKIT_MASK56, 56);
}

static inline uint32_t mdkit_mid(uint64_t a)
{
    return (uint32_t)((a >> 24) & MDKIT_MASK24);
}

static inline uint32_t mdkit_low(uint64_t a)
{
    return (uint32_t)(a & MDKIT_MASK24);
}

static inline uint64_t mdkit_from24(uint32_t w)
{
    return ((uint64_t)(mdkit_s24(w) * (INT64_C(1) << 24))) & MDKIT_MASK56;
}

static inline uint64_t mdkit_from48(uint64_t w)
{
    return (uint64_t)mdkit_signed_bits(w & MDKIT_MASK48, 48) & MDKIT_MASK56;
}

static inline uint64_t mdkit_to48(uint64_t a)
{
    int64_t v = mdkit_s56(a);
    if (v < -INT64_C(0x800000000000)) v = -INT64_C(0x800000000000);
    if (v >  INT64_C(0x7fffffffffff)) v =  INT64_C(0x7fffffffffff);
    return (uint64_t)v & MDKIT_MASK48;
}

static inline uint64_t mdkit_setmid(uint64_t a, uint32_t w)
{
    return (a & ~(UINT64_C(0xffffff) << 24)) |
           ((uint64_t)(w & MDKIT_MASK24) << 24);
}

static inline uint64_t mdkit_add(uint64_t a, uint64_t b)
{
    return (a + b) & MDKIT_MASK56;
}

static inline uint64_t mdkit_sub(uint64_t a, uint64_t b)
{
    return (a - b) & MDKIT_MASK56;
}

static inline uint64_t mdkit_asl(uint64_t a, unsigned n)
{
    return (a << n) & MDKIT_MASK56;
}

static inline uint64_t mdkit_asr(uint64_t a, unsigned n)
{
    const int64_t v = mdkit_s56(a);
    const int64_t sh = v >= 0 ? v / (INT64_C(1) << n)
        : -((-v + (INT64_C(1) << n) - 1) / (INT64_C(1) << n));
    return (uint64_t)sh & MDKIT_MASK56;
}

static inline uint64_t mdkit_mpy(uint32_t a, uint32_t b)
{
    return (uint64_t)(mdkit_s24(a) * mdkit_s24(b) * 2) & MDKIT_MASK56;
}

static inline uint64_t mdkit_mpyr(uint32_t a, uint32_t b)
{
    uint64_t acc = mdkit_mpy(a, b);
    acc = (acc + (UINT64_C(1) << 23)) & MDKIT_MASK56;
    if ((acc & MDKIT_MASK24) == 0) acc &= ~(UINT64_C(1) << 24);
    return acc & ~UINT64_C(0xffffff);
}

static inline uint64_t mdkit_macr(uint64_t acc, uint32_t x, uint32_t y)
{
    uint64_t r = (acc + mdkit_mpy(x, y)) & MDKIT_MASK56;
    r = (r + (UINT64_C(1) << 23)) & MDKIT_MASK56;
    if ((r & MDKIT_MASK24) == 0) r &= ~(UINT64_C(1) << 24);
    return r & ~UINT64_C(0xffffff);
}

static inline uint64_t mdkit_and_mid(uint64_t a, uint32_t mask)
{
    return mdkit_setmid(a, mdkit_mid(a) & mask);
}

static inline uint64_t mdkit_pack48(uint32_t hi, uint32_t lo)
{
    return ((uint64_t)(hi & MDKIT_MASK24) << 24) | (lo & MDKIT_MASK24);
}

static inline uint32_t mdkit_acc_to_word(uint64_t a)
{
    const int64_t v = mdkit_s56(a);
    if (v < -(INT64_C(0x800000) << 24)) return UINT32_C(0x800000);
    if (v >  (INT64_C(0x7fffff) << 24)) return UINT32_C(0x7fffff);
    return mdkit_mid(a);
}
#define MDKIT_TWO_PI 6.283185307179586476925286766559
#define MDKIT_PI     3.14159265358979323846

/* ---- Local coefficient construction ------------------------------------ */

static double mdkit_exp2(double x)
{
    /* x is non-negative here. Split 2^x = 2^n * 2^f and evaluate the
     * fractional part with a short exp(f*ln2) series. */
    long long n = (long long)x;
    const double f = x - (double)n;
    const double y = f * 0.69314718055994530942;
    double term = 1.0, sum = 1.0;
    for (unsigned k = 1; k <= 14; ++k) {
        term *= y / (double)k;
        sum += term;
    }
    double scale = 1.0;
    while (n > 0) { scale *= 2.0; --n; }
    return sum * scale;
}

static double mdkit_sin(double x)
{
    x -= MDKIT_TWO_PI * (double)(long long)(x / MDKIT_TWO_PI
                                            + (x < 0.0 ? -0.5 : 0.5));
    if (x > MDKIT_PI / 2.0) x = MDKIT_PI - x;
    else if (x < -MDKIT_PI / 2.0) x = -MDKIT_PI - x;
    const double x2 = x * x;
    return x * (1.0 - x2 * (1.0 / 6.0 - x2 * (1.0 / 120.0 - x2 * (1.0 / 5040.0
        - x2 * (1.0 / 362880.0 - x2 * (1.0 / 39916800.0))))));
}

static void xt_build_tables(mdkit_efm_xt *s)
{
    for (unsigned i = 0; i < 128; ++i) {
        const double v = 2048.0 * mdkit_exp2((double)i / 127.5);
        const long long rounded = (long long)(v + 0.5);
        s->pitch[i] = (uint32_t)rounded & MDKIT_MASK24;
    }

    /* XT indexes 147..472 for the three decay controls. A quartic in log2
     * space reproduces that range; the maximum construction error is 3 LSB
     * (3.6e-7 full scale). */
    static const double coef[5] = {
        16.258757166429874, -10.962508469179383, -0.13655240034698993,
        0.1686017208429095, -0.07423915973346736
    };
    for (unsigned i = 0; i < 326; ++i) {
        const double x = (double)i / 325.0;
        double exponent = coef[4];
        exponent = exponent * x + coef[3];
        exponent = exponent * x + coef[2];
        exponent = exponent * x + coef[1];
        exponent = exponent * x + coef[0];
        const double delta = mdkit_exp2(exponent);
        const long long rounded = (long long)(delta + 0.5);
        s->decay[i] = (UINT32_C(0x800000) - (uint32_t)rounded) & MDKIT_MASK24;
    }
}

static uint32_t xt_ptch(const mdkit_efm_xt *s, unsigned knob)
{
    const uint32_t raw = knob * 128u;
    const uint32_t q = raw >> 6;
    return s->pitch[q & 127u] << ((q >> 7) + 2u);
}

static uint32_t xt_decay(const mdkit_efm_xt *s, unsigned knob, unsigned rate)
{
    const uint32_t raw = knob * 128u;
    const unsigned index = 147u + ((raw * rate) >> 12);
    return s->decay[index - 147u] & MDKIT_MASK24;
}

/* ---- Control clock ------------------------------------------------------ */

static void efm_clock_seed(mdkit_control_clock *c, unsigned count,
                          const unsigned char *knobs)
{
    c->count = count;
    for (unsigned i = 0; i < count; ++i) {
        const float v = (float)knobs[i];
        c->target[i] = c->raw[i] = c->current[i] = c->pending[i] = v;
    }
    c->block = 0;
    c->next_update = 0;
    c->publish_block = 0;
    c->pending_valid = 0;
}

static void efm_clock_target(mdkit_control_clock *c, unsigned index,
                            unsigned value)
{
    if (index < c->count)
        c->target[index] = (float)(value > 127u ? 127u : value);
}

static void efm_clock_boundary(mdkit_control_clock *c)
{
    if (c->pending_valid && c->block >= c->publish_block) {
        for (unsigned i = 0; i < c->count; ++i) c->current[i] = c->pending[i];
        c->pending_valid = 0;
    }
    if (c->block == c->next_update) {
        for (unsigned i = 0; i < c->count; ++i) {
            c->pending[i] = c->raw[i];
            c->raw[i] = (3.0f * c->raw[i] + c->target[i]) * 0.25f;
        }
        c->pending_valid = 1;
        c->publish_block = c->block + 1;
        c->next_update += 12;
    }
    c->block += 1;
}

static uint32_t xt_sine(uint16_t index)
{
    const double phase = (MDKIT_TWO_PI * (double)(index & 0x7fffu)) / 32768.0;
    const double scaled = 8388608.0 * mdkit_sin(phase);
    long long sample = (long long)(scaled < 0.0 ? scaled - 0.5 : scaled + 0.5);
    if (sample > 0x7fffff) sample = 0x7fffff;
    if (sample < -0x800000) sample = -0x800000;
    return (uint32_t)sample & MDKIT_MASK24;
}

static void xt_core(const uint32_t y[9], uint32_t *fb, uint32_t *op1,
                    uint32_t *op2, uint64_t *l1, uint64_t *l2, uint64_t *l3,
                    uint32_t out[32])
{
    uint32_t x1 = y[1], x2 = y[2], x3 = y[3], x4 = y[4];
    uint32_t x5 = y[5], x6 = y[6], x8 = y[8];
    uint32_t x0f = *fb, x10 = *op1, x11 = *op2;
    uint64_t lb = *l1, ld = *l2, l12 = *l3;
    for (unsigned i = 0; i < 32; ++i) {
        uint32_t y1 = x0f, x0 = x8;
        uint64_t b = mdkit_mpy(x0, y1);
        uint32_t t1 = x1;
        uint64_t a = mdkit_mpy(y1, t1);
        x0f = mdkit_mid(b);
        b = mdkit_asl(a, 3);
        b = mdkit_add(b, mdkit_from24(t1));
        uint32_t y0 = x3;
        y1 = mdkit_mid(b);
        a = mdkit_mpy(y1, y0);
        const uint64_t xx = l12;
        a = mdkit_asr(a, 5);
        b = mdkit_from48(lb);
        b = mdkit_add(b, mdkit_from48(xx));
        b = mdkit_and_mid(b, 0x7fff);
        const uint16_t idx1 = (uint16_t)mdkit_mid(b);
        b = mdkit_sub(b, mdkit_from48(xx));
        b = mdkit_add(b, a);
        y0 = x6;
        x0 = xt_sine(idx1) & MDKIT_MASK24;
        lb = mdkit_pack48(mdkit_mid(b), mdkit_low(b));
        a = mdkit_mpy(y0, x0);
        y0 = x10;
        a = mdkit_asr(a, 8);
        t1 = x5;
        l12 = mdkit_to48(a);
        b = mdkit_mpy(t1, y0);
        a = mdkit_mpyr(y0, x0);
        x10 = mdkit_mid(b);
        x0 = mdkit_mid(a);
        b = mdkit_mpyr(x0, y1);
        y0 = x4;
        t1 = mdkit_mid(b);
        b = mdkit_mpy(t1, y0);
        b = mdkit_asl(b, 3);
        b = mdkit_add(b, mdkit_from24(y1));
        a = mdkit_from48(ld);
        b = mdkit_asr(b, 8);
        const uint16_t idx2 = (uint16_t)mdkit_mid(a);
        a = mdkit_add(a, b);
        a = mdkit_and_mid(a, 0x7fff);
        x0 = xt_sine(idx2) & MDKIT_MASK24;
        ld = mdkit_pack48(mdkit_mid(a), mdkit_low(a));
        y0 = x11;
        y1 = x2;
        b = mdkit_mpy(y1, y0);
        a = mdkit_mpyr(y0, x0);
        x11 = mdkit_mid(b);
        out[i] = mdkit_mid(a);
    }
    *fb = x0f; *op1 = x10; *op2 = x11; *l1 = lb; *l2 = ld; *l3 = l12;
}

static void xt_tail(const uint32_t in[32], uint32_t out[32], uint32_t coef,
                    uint32_t *history, uint32_t *delay)
{
    const uint32_t gain = mdkit_mid(mdkit_add(mdkit_asr(mdkit_from24(coef), 1),
                                        mdkit_from24(0x400000)));
    uint32_t hist = *history, previous = *delay;
    for (unsigned i = 0; i < 32; ++i) {
        uint64_t a = mdkit_mpy(in[i], gain);
        const uint32_t old = hist;
        hist = mdkit_mid(a);
        a = mdkit_sub(a, mdkit_from24(old));
        a = mdkit_macr(a, previous, coef);
        out[i] = mdkit_acc_to_word(a);
        previous = out[i];
    }
    *history = hist;
    *delay = previous;
}

static void xt_build_slots(const mdkit_efm_xt *s, const float k[MDKIT_EFM_XT_CONTROLS],
                           uint32_t y[9])
{
    const unsigned ptch = (unsigned)k[MDKIT_EFM_XT_PTCH] & 127u;
    const unsigned dec  = (unsigned)k[MDKIT_EFM_XT_DEC] & 127u;
    const unsigned rdec = (unsigned)k[MDKIT_EFM_XT_RDEC] & 127u;
    const unsigned mdec = (unsigned)k[MDKIT_EFM_XT_MDEC] & 127u;

    y[1] = xt_ptch(s, ptch);
    y[2] = xt_decay(s, dec, 70u);
    y[3] = ((unsigned)k[MDKIT_EFM_XT_MFRQ] << 16);
    if (y[3] < UINT32_C(0x10000)) y[3] = UINT32_C(0x10000);
    y[4] = ((unsigned)k[MDKIT_EFM_XT_MOD] << 16);
    y[5] = xt_decay(s, mdec, 82u);
    y[6] = UINT32_C(0x7a1200);
    y[7] = ((UINT32_C(0xf00000) + ((unsigned)k[MDKIT_EFM_XT_RAMP] << 16))) & MDKIT_MASK24;
    y[8] = xt_decay(s, rdec, 75u);
}

/* ---- Default controls --------------------------------------------------- */

static const unsigned char mdkit_efm_xt_defaults[MDKIT_EFM_XT_CONTROLS] = {
    25, 50, 127, 111, 5, 4, 79, 0
};

static void mdkit_efm_xt_init(void *state)
{
    mdkit_efm_xt *s = (mdkit_efm_xt *)state;
    efm_clock_seed(&s->clock, MDKIT_EFM_XT_CONTROLS, mdkit_efm_xt_defaults);
    s->pos = 0;
    xt_build_tables(s);
    for (unsigned i = 0; i < 8; ++i) s->slot[i + 1] = 0;
    xt_build_slots(s, s->clock.current, s->slot);
    s->tail_coef = 0x7de290;
    s->fb = s->op1_out = s->op2_out = 0;
    s->acc1 = s->acc2 = s->acc3 = 0;
    s->tail_hist = s->tail_delay = 0;
    s->have_block = 0;
}

static void mdkit_efm_xt_set_control(void *state, unsigned index, unsigned value)
{
    efm_clock_target(&((mdkit_efm_xt *)state)->clock, index, value);
}

static void mdkit_efm_xt_trigger(void *state)
{
    mdkit_efm_xt *s = (mdkit_efm_xt *)state;
    s->fb      = s->slot[7];
    s->op2_out = UINT32_C(0x400000) + s->op2_out;
    s->op1_out = UINT32_C(0x7fffff);
    const uint32_t clic_phase = ((unsigned)s->clock.current[MDKIT_EFM_XT_CLIC] << 6) &
                                MDKIT_MASK24;
    s->acc1 = mdkit_pack48(clic_phase & MDKIT_MASK24, 0);
    s->acc2 = mdkit_pack48(clic_phase & MDKIT_MASK24, 0);
    s->acc3 = 0;
    s->have_block = 0;
}

static void mdkit_efm_xt_render(void *state, float *out, unsigned frames)
{
    mdkit_efm_xt *s = (mdkit_efm_xt *)state;
    while (frames) {
        if (s->pos == 0u) {
            efm_clock_boundary(&s->clock);
            xt_build_slots(s, s->clock.current, s->slot);
            s->have_block = 0;
        }
        if (!s->have_block) {
            xt_core(s->slot, &s->fb, &s->op1_out, &s->op2_out,
                    &s->acc1, &s->acc2, &s->acc3, s->block);
            xt_tail(s->block, s->block, s->tail_coef, &s->tail_hist, &s->tail_delay);
            s->have_block = 1;
        }
        unsigned n = 32u - s->pos;
        if (n > frames) n = frames;
        for (unsigned i = 0; i < n; ++i) {
            const uint32_t w = s->block[s->pos + i] & MDKIT_MASK24;
            out[i] = (float)(int32_t)((w & 0x800000u)
                ? (int32_t)w - 0x1000000 : (int32_t)w) / 8388608.0f;
        }
        out += n;
        frames -= n;
        s->pos += n;
        if (s->pos >= 32u) s->pos = 0;
    }
}

static const char *const mdkit_efm_xt_control_names[MDKIT_EFM_XT_CONTROLS] = {
    "PTCH", "DEC", "RAMP", "RDEC", "MOD", "MFRQ", "MDEC", "CLIC"
};

const mdkit_voice mdkit_voice_efm_xt = {
    "EFM-XT", 34u, MDKIT_EFM_XT_CONTROLS, mdkit_efm_xt_control_names,
    sizeof(mdkit_efm_xt),
    mdkit_efm_xt_init, mdkit_efm_xt_set_control, mdkit_efm_xt_trigger,
    mdkit_efm_xt_render, mdkit_efm_xt_defaults
};
