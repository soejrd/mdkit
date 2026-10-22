/* SPDX-License-Identifier: CC0-1.0
 * mdkit_efm_cp.c - EFM-CP standalone voice
 */
#include <stdint.h>
#include <stddef.h>
#include "mdkit_efm_cp.h"


/* ---- Fixed-point helpers ------------------------------------------------
 *
 * 24-bit signed words with 56-bit accumulators. Products enter the high half
 * of the accumulator; rounded operations round the complete sum.
 */
#define MDKIT_MASK24 UINT32_C(0xffffff)
#define MDKIT_MASK48 UINT64_C(0xffffffffffff)
#define MDKIT_MASK56 UINT64_C(0xffffffffffffff)
#define MDKIT_TWO_PI 6.283185307179586476925286766559
#define MDKIT_PI     3.14159265358979323846

static inline __attribute__((unused)) int64_t mdkit_signed_bits(uint64_t word,unsigned bits)
{
    const uint64_t sign=UINT64_C(1)<<(bits-1);
    const uint64_t mask=(UINT64_C(1)<<bits)-1;
    word&=mask;
    return (word&sign)?(int64_t)word-(int64_t)(UINT64_C(1)<<bits):(int64_t)word;
}
static inline __attribute__((unused)) int64_t mdkit_s24(uint32_t w)
{ return mdkit_signed_bits(w&MDKIT_MASK24,24); }
static inline __attribute__((unused)) int64_t mdkit_s56(uint64_t a)
{ return mdkit_signed_bits(a&MDKIT_MASK56,56); }
static inline __attribute__((unused)) uint32_t mdkit_mid(uint64_t a)
{ return (uint32_t)((a>>24)&MDKIT_MASK24); }
static inline __attribute__((unused)) uint32_t mdkit_low(uint64_t a)
{ return (uint32_t)(a&MDKIT_MASK24); }
static inline __attribute__((unused)) uint64_t mdkit_from24(uint32_t w)
{ return ((uint64_t)(mdkit_s24(w)*(INT64_C(1)<<24)))&MDKIT_MASK56; }
static inline __attribute__((unused)) uint64_t mdkit_from48(uint64_t w)
{ return (uint64_t)mdkit_signed_bits(w&MDKIT_MASK48,48)&MDKIT_MASK56; }
static inline __attribute__((unused)) uint64_t mdkit_to48(uint64_t a)
{
    int64_t v=mdkit_s56(a);
    if(v < -INT64_C(0x800000000000)) v=-INT64_C(0x800000000000);
    if(v >  INT64_C(0x7fffffffffff)) v= INT64_C(0x7fffffffffff);
    return (uint64_t)v&MDKIT_MASK48;
}
static inline __attribute__((unused)) uint64_t mdkit_setmid(uint64_t a,uint32_t w)
{ return (a&~(UINT64_C(0xffffff)<<24))|((uint64_t)(w&MDKIT_MASK24)<<24); }
static inline __attribute__((unused)) uint64_t mdkit_add(uint64_t a,uint64_t b)
{ return (a+b)&MDKIT_MASK56; }
static inline __attribute__((unused)) uint64_t mdkit_sub(uint64_t a,uint64_t b)
{ return (a-b)&MDKIT_MASK56; }
static inline __attribute__((unused)) uint64_t mdkit_asl(uint64_t a,unsigned n)
{ return (a<<n)&MDKIT_MASK56; }
static inline __attribute__((unused)) uint64_t mdkit_asr(uint64_t a,unsigned n)
{
    const int64_t v=mdkit_s56(a);
    const int64_t sh=v>=0?v/(INT64_C(1)<<n):-((-v+(INT64_C(1)<<n)-1)/(INT64_C(1)<<n));
    return (uint64_t)sh&MDKIT_MASK56;
}
static inline __attribute__((unused)) uint64_t mdkit_mpy(uint32_t a,uint32_t b)
{ return (uint64_t)(mdkit_s24(a)*mdkit_s24(b)*2)&MDKIT_MASK56; }
static inline __attribute__((unused)) uint64_t mdkit_mpy_su(uint32_t s,uint32_t u)
{ return (uint64_t)(mdkit_s24(s)*(int64_t)(u&MDKIT_MASK24)*2)&MDKIT_MASK56; }
static inline __attribute__((unused)) uint64_t mdkit_mpyr(uint32_t a,uint32_t b)
{
    uint64_t acc=mdkit_mpy(a,b);
    acc=(acc+(UINT64_C(1)<<23))&MDKIT_MASK56;
    if((acc&MDKIT_MASK24)==0) acc&=~(UINT64_C(1)<<24);
    return acc&~UINT64_C(0xffffff);
}
static inline __attribute__((unused)) uint64_t mdkit_mac(uint64_t acc,uint32_t x,uint32_t y)
{ return (acc+mdkit_mpy(x,y))&MDKIT_MASK56; }
static inline __attribute__((unused)) uint64_t mdkit_macr(uint64_t acc,uint32_t x,uint32_t y)
{
    uint64_t r=(acc+mdkit_mpy(x,y))&MDKIT_MASK56;
    r=(r+(UINT64_C(1)<<23))&MDKIT_MASK56;
    if((r&MDKIT_MASK24)==0) r&=~(UINT64_C(1)<<24);
    return r&~UINT64_C(0xffffff);
}
static inline __attribute__((unused)) uint64_t mdkit_and_mid(uint64_t a,uint32_t mask)
{ return mdkit_setmid(a,mdkit_mid(a)&mask); }
static inline __attribute__((unused)) uint64_t mdkit_pack48(uint32_t hi,uint32_t lo)
{ return ((uint64_t)(hi&MDKIT_MASK24)<<24)|(lo&MDKIT_MASK24); }
static inline __attribute__((unused)) uint32_t mdkit_acc_to_word(uint64_t a)
{
    const int64_t v=mdkit_s56(a);
    if(v<-(INT64_C(0x800000)<<24)) return UINT32_C(0x800000);
    if(v> (INT64_C(0x7fffff)<<24)) return UINT32_C(0x7fffff);
    return mdkit_mid(a);
}

/* ---- Local coefficient construction ------------------------------------ */

static __attribute__((unused)) double mdkit_exp2(double x)
{
    long long n=(long long)x;
    double f=x-(double)n;
    if(f<0.0){f+=1.0;--n;}
    const double y=f*0.69314718055994530942;
    double term=1.0,sum=1.0;
    for(unsigned k=1;k<=12;++k){term*=y/(double)k;sum+=term;}
    double scale=1.0;
    while(n>0){scale*=2.0;--n;}
    while(n<0){scale*=0.5;++n;}
    return sum*scale;
}

static __attribute__((unused)) double mdkit_sin(double x)
{
    x-=MDKIT_TWO_PI*(double)(long long)(x/MDKIT_TWO_PI+(x<0.0?-0.5:0.5));
    if(x> MDKIT_PI/2.0) x= MDKIT_PI-x;
    else if(x<-MDKIT_PI/2.0) x=-MDKIT_PI-x;
    const double x2=x*x;
    return x*(1.0-x2*(1.0/6.0-x2*(1.0/120.0-x2*(1.0/5040.0
        -x2*(1.0/362880.0-x2*(1.0/39916800.0))))));
}

static __attribute__((unused)) uint32_t mdkit_logpoly(const double *c,unsigned degree,double x)
{
    double y=c[degree];
    for(unsigned i=degree;i-->0;) y=y*x+c[i];
    const double v=mdkit_exp2(y);
    return (uint32_t)(long long)(v+0.5)&MDKIT_MASK24;
}

static const double mdkit_decay_lo[4] = {
    16.2587772550628955,-1.65240088787418893,
    -0.00404324436830615259,0.000986419366542184073};
static const double mdkit_decay_mid[4] = {
    14.5695368124188676,-5.1434733494674223,
    -0.00676881681461822659,0.00248508759982288534};
static const double mdkit_decay_hi[4] = {
    9.38745457862932398,-4.26718908320820933,
    0.00385140734226158653,-0.00582524258210295829};
static const double mdkit_decay_end[4] = {
    5.08388492206188403,-1.11841448721353331,
    0.0978243040805674563,-0.0695259770934334581};

static __attribute__((unused)) uint32_t mdkit_decay_index(unsigned index)
{
    if(index<=196u)
        return (UINT32_C(0x800000)-mdkit_logpoly(mdkit_decay_lo,3,((double)index-147.0)/49.0))&MDKIT_MASK24;
    if(index<=349u)
        return (UINT32_C(0x800000)-mdkit_logpoly(mdkit_decay_mid,3,(double)(index-197u)/152.0))&MDKIT_MASK24;
    if(index<=476u)
        return (UINT32_C(0x800000)-mdkit_logpoly(mdkit_decay_hi,3,(double)(index-350u)/126.0))&MDKIT_MASK24;
    return (UINT32_C(0x800000)-mdkit_logpoly(mdkit_decay_end,3,(double)(index-477u)/32.0))&MDKIT_MASK24;
}
static __attribute__((unused)) uint32_t mdkit_decay_rate(unsigned knob,unsigned rate)
{ return mdkit_decay_index(147u+(((uint32_t)knob*128u*rate)>>12)); }

static __attribute__((unused)) uint32_t mdkit_pitch_poly(const double *c,unsigned degree,unsigned knob)
{ return mdkit_logpoly(c,degree,(double)knob/127.0); }
static __attribute__((unused)) uint32_t mdkit_affine(unsigned knob,unsigned shift)
{ return ((uint32_t)knob<<shift)&MDKIT_MASK24; }
static __attribute__((unused)) uint32_t mdkit_affine_min(unsigned knob,unsigned shift,uint32_t minimum)
{
    const uint32_t v=((uint32_t)knob<<shift)&MDKIT_MASK24;
    return v<minimum?minimum:v;
}
static __attribute__((unused)) uint32_t mdkit_inverse(unsigned knob,unsigned shift)
{ return (UINT32_C(0x7fffff)-((uint32_t)knob<<shift))&MDKIT_MASK24; }
static __attribute__((unused)) uint32_t mdkit_affine_offset(unsigned knob,int64_t step,int64_t offset)
{ return (uint32_t)(((int64_t)knob*step+offset)&0xffffff); }


/* ---- Control clock ------------------------------------------------------ */

static void efm_clock_seed(mdkit_control_clock *c,unsigned count,
                           const unsigned char *knobs)
{
    c->count=count;
    for(unsigned i=0;i<count;++i) {
        const float v=(float)knobs[i];
        c->target[i]=c->raw[i]=c->current[i]=c->pending[i]=v;
    }
    c->block=0; c->next_update=0; c->publish_block=0; c->pending_valid=0;
}

static void efm_clock_target(mdkit_control_clock *c,unsigned index,unsigned value)
{
    if(index<c->count) c->target[index]=(float)(value>127u?127u:value);
}

static void efm_clock_boundary(mdkit_control_clock *c)
{
    if(c->pending_valid && c->block>=c->publish_block) {
        for(unsigned i=0;i<c->count;++i) c->current[i]=c->pending[i];
        c->pending_valid=0;
    }
    if(c->block==c->next_update) {
        for(unsigned i=0;i<c->count;++i) {
            c->pending[i]=c->raw[i];
            c->raw[i]=(3.0f*c->raw[i]+c->target[i])*0.25f;
        }
        c->pending_valid=1;
        c->publish_block=c->block+1;
        c->next_update+=12;
    }
    c->block+=1;
}

/* Accurate sine at the ABI boundary: same range reduction as mdkit_sin, one
 * more Taylor term, so the quantized 24-bit sample matches the implementation
 * mathematical sine on every phase-grid point (no fitted constants; the
 * coefficients are the exact 1/(2k+1)! series terms). */
static uint32_t cp_sine(uint16_t index)
{
    double x = (MDKIT_TWO_PI * (double)(index & 0x7fffu)) / 32768.0;
    x -= MDKIT_TWO_PI * (double)(long long)(x / MDKIT_TWO_PI
                                               + (x < 0.0 ? -0.5 : 0.5));
    if (x >  MDKIT_PI / 2.0) x =  MDKIT_PI - x;
    else if (x < -MDKIT_PI / 2.0) x = -MDKIT_PI - x;
    const double x2 = x * x;
    const double s = x * (1.0 - x2 * (1.0 / 6.0 - x2 * (1.0 / 120.0
        - x2 * (1.0 / 5040.0 - x2 * (1.0 / 362880.0 - x2 * (1.0 / 39916800.0
        - x2 * (1.0 / 6227020800.0 - x2 * (1.0 / 1307674368000.0))))))));
    const double scaled = 8388608.0 * s;
    long long sample = (long long)(scaled < 0.0 ? scaled - 0.5 : scaled + 0.5);
    if (sample > 0x7fffff) sample = 0x7fffff;
    if (sample < -0x800000) sample = -0x800000;
    return (uint32_t)sample & MDKIT_MASK24;
}

static void cp_resonance(const uint32_t in[32], uint32_t out[32], uint32_t coefficient,
                         uint32_t *history, uint32_t *delay)
{
    const uint32_t gain = mdkit_mid(mdkit_add(mdkit_asr(mdkit_from24(coefficient), 1),
                                        mdkit_from24(0x400000)));
    uint32_t hist = *history, previous = *delay;
    for (unsigned i = 0; i < 32; ++i) {
        uint64_t a = mdkit_mpy(in[i], gain);
        const uint32_t old = hist;
        hist = mdkit_mid(a);
        a = mdkit_sub(a, mdkit_from24(old));
        a = mdkit_mac(a, previous, coefficient);
        out[i] = mdkit_acc_to_word(a);
        previous = out[i];
    }
    *history = hist;
    *delay = previous;
}

static const double mdkit_cp_pitch[7] = { 14.501242901620763, 5.429612780818404, 0.1998103824399453, -0.6913575527820134, 1.235690484369834, -1.094622722743193, 0.3783817650447076 };
static void cp_build_controls(mdkit_efm_cp *s)
{
    const float *k=s->clock.current;
    s->op1_rate=mdkit_pitch_poly(mdkit_cp_pitch,6,(unsigned)k[MDKIT_EFM_CP_PTCH]);
    s->op1_decay=mdkit_decay_rate((unsigned)k[MDKIT_EFM_CP_DEC],70u);
    s->mod_rate=mdkit_affine((unsigned)k[MDKIT_EFM_CP_MFRQ],14u);
    if(s->mod_rate<0x4000u) s->mod_rate=0x4000u;
    s->mod_amount=mdkit_affine((unsigned)k[MDKIT_EFM_CP_MOD],16u);
    s->fb_decay=mdkit_decay_rate((unsigned)k[MDKIT_EFM_CP_MDEC],82u);
    s->clipper_len=(unsigned)k[MDKIT_EFM_CP_CLPS]/8u;
    s->clipper_decay=mdkit_decay_index(70u+(((uint32_t)(unsigned)k[MDKIT_EFM_CP_CDEC]*128u*40u)>>12));
    s->tail_coef=mdkit_inverse((unsigned)k[MDKIT_EFM_CP_HPF],15u);
}

/* ---- Default controls --------------------------------------------------- */

static const unsigned char mdkit_efm_cp_defaults[MDKIT_EFM_CP_CONTROLS] = {
    50, 40, 127, 24, 115, 36, 87, 8
};

static void mdkit_efm_cp_init(void *state)
{
    mdkit_efm_cp *s = (mdkit_efm_cp *)state;
    efm_clock_seed(&s->clock, MDKIT_EFM_CP_CONTROLS, mdkit_efm_cp_defaults);
    s->pos = 0;
    s->mod_index = 0x7a1200u;
    s->fb_coef   = 0;
    cp_build_controls(s);
    s->l_c = s->l_e = s->l_13 = 0;
    s->op1_out = s->op2_out = 0;
    /* Fresh DSP allocation is zero: the clipper counter/coefficient are armed
     * by the trigger, not by init (so the voice is silent before a trigger). */
    s->clip_b = 0;
    s->clip_env = 0;
    s->res_hist = s->res_delay = 0;
    s->have_block = 0;
}

static void mdkit_efm_cp_set_control(void *state, unsigned i, unsigned v)
{
    efm_clock_target(&((mdkit_efm_cp *)state)->clock, i, v);
}

static void mdkit_efm_cp_trigger(void *state)
{
    /* ..(trigger): phases and modulator clear, operator-1 out <- MOD
     * coefficient (0), operator-2 out latches full scale, clipper counter <-
     * CLPS and running coefficient <- 0x50c335; output resonator reset. */
    mdkit_efm_cp *s = (mdkit_efm_cp *)state;
    cp_build_controls(s);   /* latch the published coefficients, as prepare() does */
    s->l_c = s->l_e = s->l_13 = 0;
    s->op1_out = 0;
    s->op2_out = 0x7fffffu;
    s->clip_env = 0x50c335u;
    s->clip_b = s->clipper_len;
    s->res_hist = s->res_delay = 0;
    s->have_block = 0;
}

static void mdkit_efm_cp_render(void *state, float *out, unsigned frames)
{
    mdkit_efm_cp *s = (mdkit_efm_cp *)state;
    while (frames) {
        if (s->pos == 0u) {
            efm_clock_boundary(&s->clock);
            cp_build_controls(s);
            s->have_block = 0;
        }
        if (!s->have_block) {
            uint32_t ring[32];
            uint64_t lc = s->l_c, le = s->l_e, l13 = s->l_13;
            uint32_t op1 = s->op1_out, op2 = s->op2_out;

            /* Implementation detail. */
            for (unsigned i = 0; i < 32; ++i) {
                uint32_t y1 = op1;                  /* x0x10 operator-1 out */
                uint32_t x0 = s->fb_coef;           /* x8  fixed 0 */
                uint64_t b = mdkit_mpy(x0, y1);
                uint32_t x1 = s->op1_rate;          /* x1  PTCH */
                uint64_t a = mdkit_mpy(y1, x1);
                op1 = mdkit_mid(b);
                b = mdkit_asl(a, 3);
                b = mdkit_add(b, mdkit_from24(x1));
                uint32_t y0 = s->mod_rate;          /* x3  MFRQ */
                y1 = mdkit_mid(b);
                a = mdkit_mpy(y1, y0);
                const uint64_t xx = l13;
                a = mdkit_asr(a, 5);
                b = mdkit_from48(lc);
                b = mdkit_add(b, mdkit_from48(xx));
                b = mdkit_and_mid(b, 0x7fff);
                const uint16_t index1 = (uint16_t)mdkit_mid(b);
                b = mdkit_sub(b, mdkit_from48(xx));
                b = mdkit_add(b, a);
                y0 = s->mod_index;                  /* x6  fixed modulator idx */
                x0 = cp_sine(index1) & MDKIT_MASK24;
                lc = mdkit_pack48(mdkit_mid(b), mdkit_low(b));
                a = mdkit_mpy(y0, x0);
                y0 = op2;                           /* x0x11 operator-2 out */
                a = mdkit_asr(a, 8);
                x1 = s->fb_decay;                   /* x5  MDEC */
                l13 = mdkit_to48(a);
                b = mdkit_mpy(x1, y0);
                a = mdkit_mpyr(y0, x0);
                op2 = mdkit_mid(b);
                x0 = mdkit_mid(a);
                b = mdkit_mpyr(x0, y1);
                y0 = s->mod_amount;                 /* x4  MOD */
                x1 = mdkit_mid(b);
                b = mdkit_mpy(x1, y0);
                b = mdkit_asl(b, 3);
                b = mdkit_add(b, mdkit_from24(y1));
                a = mdkit_from48(le);
                b = mdkit_asr(b, 8);
                const uint16_t index2 = (uint16_t)mdkit_mid(a);
                a = mdkit_add(a, b);
                a = mdkit_and_mid(a, 0x7fff);
                x0 = cp_sine(index2) & MDKIT_MASK24;
                le = mdkit_pack48(mdkit_mid(a), mdkit_low(a));
                ring[i] = x0;                       /* no final operator stage */
            }

            /* clipper , in place. x0 = CDEC (fallback DEC),
             * counter b = CLPS, running coefficient y0 = clip_env. */
            {
                uint32_t x0 = s->clipper_decay;     /* y0xb */
                uint32_t b = s->clip_b;             /* y0x15 counter */
                if (b == 0) x0 = s->op1_decay;      /* y2 = DEC */
                const uint32_t lim = 0x66666u;
                uint32_t y0 = s->clip_env;          /* y0x12 running coeff */
                for (unsigned i = 0; i < 32; ++i) {
                    const uint32_t y1 = ring[i];
                    const uint64_t bb = mdkit_mpy(y0, x0);
                    const uint64_t aa = mdkit_mpyr(y1, y0);
                    y0 = mdkit_mid(bb);
                    if (b != 0 && y0 < lim) {
                        b = (b - 1) & MDKIT_MASK24;
                        y0 = 0x50c335u;
                        if (b == 0) x0 = s->op1_decay;
                    }
                    ring[i] = mdkit_mid(aa);
                }
                s->clip_b = b;
                s->clip_env = y0;
            }

            cp_resonance(ring, s->block, s->tail_coef, &s->res_hist, &s->res_delay);

            s->l_c = lc; s->l_e = le; s->l_13 = l13;
            s->op1_out = op1; s->op2_out = op2;
            s->have_block = 1;
        }
        unsigned n = 32u - s->pos;
        if (n > frames) n = frames;
        for (unsigned i = 0; i < n; ++i) {
            const uint32_t w = s->block[s->pos + i] & MDKIT_MASK24;
            const int32_t v = (w & 0x800000u) ? (int32_t)w - 0x1000000 : (int32_t)w;
            out[i] = (float)v / 8388608.0f;
        }
        out += n; frames -= n; s->pos += n;
        if (s->pos >= 32u) s->pos = 0;
    }
}

static const char *const mdkit_efm_cp_control_names[MDKIT_EFM_CP_CONTROLS] = {
    "PTCH", "DEC", "CLPS", "CDEC", "MOD", "MFRQ", "MDEC", "HPF"
};

const mdkit_voice mdkit_voice_efm_cp = {
    "EFM-CP", 35u, MDKIT_EFM_CP_CONTROLS, mdkit_efm_cp_control_names,
    sizeof(mdkit_efm_cp),
    mdkit_efm_cp_init, mdkit_efm_cp_set_control, mdkit_efm_cp_trigger, mdkit_efm_cp_render, mdkit_efm_cp_defaults
};
