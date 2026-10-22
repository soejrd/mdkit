/* SPDX-License-Identifier: CC0-1.0
 * mdkit_efm_cy.c - EFM-CY standalone voice
 */
#include <stdint.h>
#include <stddef.h>
#include "mdkit_efm_cy.h"


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
static uint32_t cy_sine(uint16_t index)
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

/* DSP two-pole resonator with the rounded MAC (CY's HP stages use macr). */
static void cy_resonance(const uint32_t in[32], uint32_t out[32], uint32_t coefficient,
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
        a = mdkit_macr(a, previous, coefficient);
        out[i] = mdkit_acc_to_word(a);
        previous = out[i];
    }
    *history = hist;
    *delay = previous;
}

/* ---- Voice control law ------------------------------------------------- */

static const double mdkit_cy_pitch[4] = { 11.500590089999303, 2.4796963378948127, -0.00549031602082840093, 0.00724890179121155491 };
static void cy_build_controls(mdkit_efm_cy *s)
{
    const float *k=s->clock.current;
    const uint32_t base=mdkit_pitch_poly(mdkit_cy_pitch,3,(unsigned)k[MDKIT_EFM_CY_PTCH]);
    const uint32_t a=361u*base,b=461u*base,d=691u*base;
    const uint32_t factor=mdkit_affine_offset((unsigned)k[MDKIT_EFM_CY_MFRQ],128,0x400);
    s->fm_rate[0]=base>>4; s->fm_rate[1]=a>>12;
    s->fm_rate[2]=b>>12; s->fm_rate[3]=d>>12;
    s->scan_step[0]=(base*factor)>>15; s->scan_step[1]=((a>>8)*factor)>>15;
    s->scan_step[2]=((b>>8)*factor)>>15; s->scan_step[3]=((d>>8)*factor)>>15;
    s->env_decay=mdkit_decay_rate((unsigned)k[MDKIT_EFM_CY_DEC],70u);
    s->wave_amp=mdkit_affine_offset((unsigned)k[MDKIT_EFM_CY_MOD],384,0xc00);
    s->wave_env_decay=mdkit_decay_index(350u+(((uint32_t)(unsigned)k[MDKIT_EFM_CY_MDEC]*128u*40u)>>12));
    s->fm_fb=mdkit_affine_offset((unsigned)k[MDKIT_EFM_CY_FB],102,0);
    s->hp_coef=mdkit_affine_offset((unsigned)k[MDKIT_EFM_CY_HPF],-23808,0x7dfffe);
    s->fb_env_decay=0x400000u;
}


/* ---- Default controls --------------------------------------------------- */

static const unsigned char mdkit_efm_cy_defaults[MDKIT_EFM_CY_CONTROLS] = {
    64, 72, 96, 92, 45, 32, 64
};

static void mdkit_efm_cy_init(void *state)
{
    mdkit_efm_cy *s = (mdkit_efm_cy *)state;
    efm_clock_seed(&s->clock, MDKIT_EFM_CY_CONTROLS, mdkit_efm_cy_defaults);
    s->pos = 0;
    cy_build_controls(s);
    for (unsigned op = 0; op < 4; ++op) {
        s->scan_ptr[op] = 0;
        s->fm_phase[op] = 0;
        s->fm_fb_reg[op] = 0;
    }
    s->wave_env = s->amp_env = 0;
    s->hp1_hist = s->hp1_delay = 0;
    s->hp2_hist = s->hp2_delay = 0;
    s->have_block = 0;
}

static void mdkit_efm_cy_set_control(void *state, unsigned i, unsigned v)
{
    efm_clock_target(&((mdkit_efm_cy *)state)->clock, i, v);
}

static void mdkit_efm_cy_trigger(void *state)
{
    /* ..(trigger): scan pointers seed 0x148000, FM phases and feedback
     * registers clear, both resonators reset. The amplitude envelope ADDS to
     * the retained value; the wave envelope latches full scale. */
    mdkit_efm_cy *s = (mdkit_efm_cy *)state;
    for (unsigned op = 0; op < 4; ++op) {
        s->scan_ptr[op] = 0x148000u;
        s->fm_phase[op] = 0;
        s->fm_fb_reg[op] = 0;
    }
    s->hp1_hist = s->hp1_delay = 0;
    s->hp2_hist = s->hp2_delay = 0;
    s->wave_env = 0x7fffffu;
    s->amp_env = mdkit_acc_to_word(mdkit_add(mdkit_from24(0x50c335), mdkit_from24(s->amp_env)));
    s->have_block = 0;
}

static void mdkit_efm_cy_render(void *state, float *out, unsigned frames)
{
    mdkit_efm_cy *s = (mdkit_efm_cy *)state;
    while (frames) {
        if (s->pos == 0u) {
            efm_clock_boundary(&s->clock);
            cy_build_controls(s);
            s->have_block = 0;
        }
        if (!s->have_block) {
            uint32_t wave[4][32];
            uint32_t mix[32], tail[32];

            /* wave-bank scans : each operator advances its
             * own circular pointer; the pointer's low 15 bits index the sine,
             * the high bits (0x148000) are the scan tag. */
            for (unsigned op = 0; op < 4; ++op) {
                uint32_t pointer = s->scan_ptr[op];
                for (unsigned i = 0; i < 32; ++i) {
                    wave[op][i] = mdkit_mid(mdkit_mpyr(s->wave_amp,
                                                 cy_sine(pointer & 0x7fffu)));
                    pointer = 0x148000u | ((pointer + s->scan_step[op]) & 0x7fffu);
                }
                s->scan_ptr[op] = pointer;
            }
            /* P:103451..103457: all four operators read the SAME old envelope. */
            uint32_t env = s->wave_env;
            for (unsigned i = 0; i < 32; ++i) {
                for (unsigned op = 0; op < 4; ++op)
                    wave[op][i] = mdkit_acc_to_word(mdkit_mpyr(env, wave[op][i]));
                env = mdkit_mid(mdkit_mpy(s->wave_env_decay, env));
            }
            s->wave_env = env;

            /* P:10345b..1034a9: FM feedback scans. Phase arithmetic stays
             * full-width until the state store. */
            for (unsigned op = 0; op < 4; ++op) {
                uint64_t phase = mdkit_from24(s->fm_phase[op]);
                uint64_t feedback = mdkit_from24(s->fm_fb_reg[op]);
                for (unsigned i = 0; i < 32; ++i) {
                    feedback = mdkit_add(feedback, mdkit_from24(wave[op][i]));
                    phase = mdkit_add(phase, feedback);
                    phase = mdkit_and_mid(phase, 0x7fff);
                    const uint32_t sample = cy_sine((uint16_t)mdkit_mid(phase));
                    phase = mdkit_sub(phase, feedback);
                    phase = mdkit_add(phase, mdkit_from24(s->fm_rate[op]));
                    wave[op][i] = sample;
                    feedback = mdkit_mpyr(sample, s->fm_fb);
                }
                s->fm_phase[op] = mdkit_mid(phase);
                s->fm_fb_reg[op] = mdkit_mid(feedback);
            }

            /* Implementation detail. */
            for (unsigned i = 0; i < 32; ++i) {
                uint64_t a = mdkit_from24(wave[0][i]);
                for (unsigned op = 1; op < 4; ++op) a = mdkit_add(a, mdkit_from24(wave[op][i]));
                mix[i] = mdkit_acc_to_word(mdkit_asr(a, 2));
            }
            env = s->amp_env;
            for (unsigned i = 0; i < 32; ++i) {
                const uint32_t gain = mdkit_acc_to_word(mdkit_asl(mdkit_mpy_su(env, s->fb_env_decay), 1));
                const uint32_t next = mdkit_mid(mdkit_mpy(s->env_decay, env));
                mix[i] = mdkit_mid(mdkit_mpyr(gain, mix[i]));
                env = next;
            }
            s->amp_env = env;

            /* Implementation detail. */
            cy_resonance(mix, tail, s->hp_coef, &s->hp1_hist, &s->hp1_delay);
            cy_resonance(tail, s->block, s->hp_coef, &s->hp2_hist, &s->hp2_delay);
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

static const char *const mdkit_efm_cy_control_names[MDKIT_EFM_CY_CONTROLS] = {
    "PTCH", "DEC", "FB", "HPF", "MOD", "MFRQ", "MDEC"
};

const mdkit_voice mdkit_voice_efm_cy = {
    "EFM-CY", 39u, MDKIT_EFM_CY_CONTROLS, mdkit_efm_cy_control_names,
    sizeof(mdkit_efm_cy),
    mdkit_efm_cy_init, mdkit_efm_cy_set_control, mdkit_efm_cy_trigger, mdkit_efm_cy_render, mdkit_efm_cy_defaults
};
