/* SPDX-License-Identifier: CC0-1.0
 * mdkit_efm_cb.c - EFM-CB standalone voice
 */
#include <stdint.h>
#include <stddef.h>
#include "mdkit_efm_cb.h"


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

#define CB_SIN_SCALE 32768.0

/* ---- Voice control law ------------------------------------------------- */

static const double mdkit_cb_pitch[9] = { 14.9976519277879614, 2.54426521195477839, -1.3251373539529292, 11.8963910478631014, -50.4242646896793829, 112.190223873393094, -135.268096365973292, 83.7314527476568742, -20.8627265763582841 };
static void cb_build_slots(const float k[MDKIT_EFM_CB_CONTROLS],mdkit_efm_cb *s)
{
    const unsigned ptch=(unsigned)k[MDKIT_EFM_CB_PTCH];
    const unsigned snap=(unsigned)k[MDKIT_EFM_CB_SNAP];
    s->pitch_a=mdkit_pitch_poly(mdkit_cb_pitch,8,ptch);
    s->pitch_c=(s->pitch_a*379u)>>8;
    s->mod_freq=mdkit_affine((unsigned)k[MDKIT_EFM_CB_MFRQ],16u);
    if(s->mod_freq<0x10000u) s->mod_freq=0x10000u;
    s->fm_index=mdkit_affine((unsigned)k[MDKIT_EFM_CB_MOD],15u);
    s->mdec=mdkit_decay_rate((unsigned)k[MDKIT_EFM_CB_MDEC],82u);
    s->fb_index=mdkit_affine((unsigned)k[MDKIT_EFM_CB_FB],0u)*24576u;
    s->env1_amp=mdkit_affine_offset(snap,-53760,0x7ffe00);
    s->dec=mdkit_decay_rate((unsigned)k[MDKIT_EFM_CB_DEC],70u);
    s->env2_amp=((uint32_t)snap*53760u)&MDKIT_MASK24;
    s->env2_decay=mdkit_decay_index(100u+(((uint32_t)(unsigned)k[MDKIT_EFM_CB_DEC]*128u*38u)>>12));
}


/* sin(index) as the implementation reads it: a 15-bit table index over 0..32768.
 * A pure function (TABLE-POLICY.md) computed with a range-reduced Taylor series
 * to x^23 so the rounded word matches the implementation libm sin for every index
 * (mdkit_sin's shorter series differs by 1 LSB on ~3% of indices, which the
 * CB feedback loop amplifies). No fitted constant; this is the same sine. */
static uint32_t cb_sine(uint16_t index)
{
    double x = (MDKIT_TWO_PI * (double)(index & 0x7fffu)) / CB_SIN_SCALE;
    x -= MDKIT_TWO_PI * (double)(long long)(x / MDKIT_TWO_PI
                                               + (x < 0.0 ? -0.5 : 0.5));
    if (x >  MDKIT_PI / 2.0) x =  MDKIT_PI - x;
    else if (x < -MDKIT_PI / 2.0) x = -MDKIT_PI - x;
    const double x2 = x * x;
    double p = 1.0 / 51090942171709440000.0;
    p = -1.0 / 121645100408832000.0 + x2 * p;
    p =  1.0 / 355687428096000.0    + x2 * p;
    p = -1.0 / 1307674368000.0      + x2 * p;
    p =  1.0 / 6227020800.0         + x2 * p;
    p = -1.0 / 39916800.0           + x2 * p;
    p =  1.0 / 362880.0             + x2 * p;
    p = -1.0 / 5040.0               + x2 * p;
    p =  1.0 / 120.0                + x2 * p;
    p = -1.0 / 6.0                  + x2 * p;
    p =  1.0                        + x2 * p;
    const double scaled = 8388608.0 * (x * p);
    long long sample = (long long)(scaled < 0.0 ? scaled - 0.5 : scaled + 0.5);
    if (sample > 0x7fffff) sample = 0x7fffff;
    if (sample < -0x800000) sample = -0x800000;
    return (uint32_t)sample & MDKIT_MASK24;
}

/* One 32-sample block of the four-operator cascade. */
static void cb_core(mdkit_efm_cb *s, uint32_t out[32])
{
    const uint32_t op_a_pitch = s->pitch_a;
    const uint32_t op_c_pitch = s->pitch_c;
    const uint32_t mod_freq   = s->mod_freq;
    const uint32_t fm_index   = s->fm_index;
    const uint32_t env_decay1 = s->dec;
    const uint32_t self_pm    = s->fb_index;
    const uint32_t env_amp1   = s->env1_amp;
    const uint32_t env_amp2   = s->env2_amp;
    const uint32_t env_decay2 = s->env2_decay;

    uint64_t phase_b = s->phase_b, phase_d = s->phase_d;
    uint64_t phase_f = s->phase_f, phase_11 = s->phase_11;
    uint64_t mod_d = s->mod_d, mod_11 = s->mod_11;
    uint32_t fb = s->fb;
    uint32_t env1 = s->env1, env2 = s->env2;

    for (unsigned i = 0; i < 32; ++i) {
        const uint32_t fb_old = fb;

        /* Implementation detail. */
        uint32_t y0 = mod_freq, y1 = op_a_pitch;
        uint64_t b = mdkit_mpy(y1, y0);
        uint64_t a = phase_b;
        b = mdkit_asr(b, 5);
        y0 = fb_old;
        a = mdkit_add(a, b);
        a = mdkit_and_mid(a, 0x7fff);
        const uint32_t sine_a = cb_sine((uint16_t)mdkit_mid(phase_b)); /* old phase */
        phase_b = mdkit_pack48(mdkit_mid(a), mdkit_low(a));
        a = mdkit_mpyr(y0, sine_a);
        uint32_t x0 = mdkit_mid(a);
        b = mdkit_mpyr(x0, y1);

        /* Implementation detail. */
        uint32_t x1 = mdkit_mid(b);
        y0 = fm_index;
        b = mdkit_mpy(x1, y0);
        b = mdkit_asl(b, 3);
        b = mdkit_add(b, mdkit_from24(y1));
        b = mdkit_asr(b, 8);
        const uint64_t t_d = mdkit_and_mid(mdkit_add(phase_d, mod_d), 0x7fff);
        a = mdkit_add(mdkit_sub(t_d, mod_d), b);
        phase_d = mdkit_pack48(mdkit_mid(a), mdkit_low(a));
        const uint32_t sine_b = cb_sine((uint16_t)mdkit_mid(t_d));
        const uint32_t env_b = sine_b;                /* this block's op B out */
        mod_d = mdkit_to48(mdkit_asr(mdkit_mpy(self_pm, sine_b), 8));

        /* Implementation detail. */
        y0 = mod_freq; y1 = op_c_pitch;
        b = mdkit_mpy(y1, y0);
        b = mdkit_asr(b, 5);
        a = phase_f;
        a = mdkit_add(a, b);
        y0 = fb_old;
        a = mdkit_and_mid(a, 0x7fff);
        const uint32_t sine_c = cb_sine((uint16_t)mdkit_mid(phase_f)); /* old phase */
        phase_f = mdkit_pack48(mdkit_mid(a), mdkit_low(a));
        x1 = s->mdec;
        b = mdkit_mpy(x1, y0);
        a = mdkit_mpyr(y0, sine_c);
        fb = mdkit_mid(b);
        x0 = mdkit_mid(a);
        b = mdkit_mpyr(x0, y1);

        /* Implementation detail. */
        x1 = mdkit_mid(b);
        y0 = fm_index;
        b = mdkit_mpy(x1, y0);
        b = mdkit_asl(b, 3);
        b = mdkit_add(b, mdkit_from24(y1));
        b = mdkit_asr(b, 8);
        const uint64_t t_11 = mdkit_and_mid(mdkit_add(phase_11, mod_11), 0x7fff);
        a = mdkit_add(mdkit_sub(t_11, mod_11), b);
        phase_11 = mdkit_pack48(mdkit_mid(a), mdkit_low(a));
        const uint32_t sine_d = cb_sine((uint16_t)mdkit_mid(t_11));
        a = mdkit_asr(mdkit_mpy(sine_d, self_pm), 8);
        mod_11 = mdkit_to48(a);

        /* Implementation detail. */
        b = mdkit_add(mdkit_from24(env_b), mdkit_from24(sine_d));
        b = mdkit_asr(b, 1);
        x0 = mdkit_mid(b);
        y0 = env1;
        b = mdkit_mpy(env_decay1, y0);
        a = mdkit_mpyr(env_amp1, y0);
        env1 = mdkit_mid(b);
        y0 = env2;
        b = mdkit_mpy(env_decay2, y0);
        const uint64_t a9 = mdkit_mpyr(env_amp2, y0);
        env2 = mdkit_mid(b);
        a = mdkit_add(a, a9);
        x1 = mdkit_mid(a);
        a = mdkit_mpyr(x1, x0);
        out[i] = mdkit_mid(a);
    }

    s->phase_b = phase_b; s->phase_d = phase_d;
    s->phase_f = phase_f; s->phase_11 = phase_11;
    s->mod_d = mod_d; s->mod_11 = mod_11;
    s->fb = fb; s->env1 = env1; s->env2 = env2;
}

/* ---- Default controls --------------------------------------------------- */

static const unsigned char mdkit_efm_cb_defaults[MDKIT_EFM_CB_CONTROLS] = {
    54, 61, 92, 56, 17, 112, 42
};

static void mdkit_efm_cb_init(void *state)
{
    mdkit_efm_cb *s = (mdkit_efm_cb *)state;
    efm_clock_seed(&s->clock, MDKIT_EFM_CB_CONTROLS, mdkit_efm_cb_defaults);
    s->pos = 0;
    s->have_block = 0;
    cb_build_slots(s->clock.current, s);
    s->phase_b = s->phase_d = s->phase_f = s->phase_11 = 0;
    s->mod_d = s->mod_11 = 0;
    s->fb = s->env1 = s->env2 = 0;
}

static void mdkit_efm_cb_set_control(void *state, unsigned i, unsigned v)
{
    efm_clock_target(&((mdkit_efm_cb *)state)->clock, i, v);
}

static void mdkit_efm_cb_trigger(void *state)
{
    mdkit_efm_cb *s = (mdkit_efm_cb *)state;
    /*  trigger (trigger): clear the phase and modulator
     * accumulators (y0xb..y0x12, y0x16..y0x19), set the feedback register to
     * 0x7fffff and both envelope states to 0x5a8279. */
    s->phase_b = s->phase_d = s->phase_f = s->phase_11 = 0;
    s->mod_d = s->mod_11 = 0;
    s->fb = 0x7fffff;
    s->env1 = s->env2 = 0x5a8279;
    s->have_block = 0;
}

static void mdkit_efm_cb_render(void *state, float *out, unsigned frames)
{
    mdkit_efm_cb *s = (mdkit_efm_cb *)state;
    while (frames) {
        if (s->pos == 0u) {
            efm_clock_boundary(&s->clock);
            cb_build_slots(s->clock.current, s);
            s->have_block = 0;
        }
        if (!s->have_block) {
            cb_core(s, s->block);
            s->have_block = 1;
        }
        unsigned n = 32u - s->pos;
        if (n > frames) n = frames;
        for (unsigned i = 0; i < n; ++i) {
            const uint32_t w = s->block[s->pos + i] & MDKIT_MASK24;
            out[i] = (float)(int32_t)((w & 0x800000u) ? (int32_t)w - 0x1000000 : (int32_t)w) / 8388608.0f;
        }
        out += n; frames -= n; s->pos += n;
        if (s->pos >= 32u) s->pos = 0;
    }
}

static const char *const mdkit_efm_cb_control_names[MDKIT_EFM_CB_CONTROLS] = {
    "PTCH", "DEC", "SNAP", "FB", "MOD", "MFRQ", "MDEC"
};

const mdkit_voice mdkit_voice_efm_cb = {
    "EFM-CB", 37u, MDKIT_EFM_CB_CONTROLS, mdkit_efm_cb_control_names,
    sizeof(mdkit_efm_cb),
    mdkit_efm_cb_init, mdkit_efm_cb_set_control, mdkit_efm_cb_trigger, mdkit_efm_cb_render, mdkit_efm_cb_defaults
};
