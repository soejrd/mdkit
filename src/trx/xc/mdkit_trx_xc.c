/* SPDX-License-Identifier: CC0-1.0
 * TRX-XC: self-contained drum synthesis.
 */
#include "mdkit_trx_xc.h"

/* ---- Default controls --------------------------------------------------- */
/* Active controls are in descriptor order; unused clock slots stay at 64. */
static const unsigned char mdkit_trx_xc_defaults[MDKIT_MAX_CONTROLS] = {
    64, 32, 32, 100, 0, 0, 0, 64
};


#define MD_MASK24 UINT32_C(0xffffff)
#define MD_MASK48 UINT64_C(0xffffffffffff)
#define MD_MASK56 UINT64_C(0xffffffffffffff)

static inline int64_t xc_signed_bits(uint64_t word, unsigned bits)
{
    const uint64_t sign = UINT64_C(1) << (bits - 1);
    const uint64_t mask = (UINT64_C(1) << bits) - 1;
    word &= mask;
    return (word & sign) ? (int64_t)word - (int64_t)(UINT64_C(1) << bits)
                         : (int64_t)word;
}
static inline int64_t xc_s24(uint32_t w) { return xc_signed_bits(w & MD_MASK24, 24); }
static inline int64_t xc_s56(uint64_t a) { return xc_signed_bits(a & MD_MASK56, 56); }
static inline uint32_t xc_mid(uint64_t a) { return (uint32_t)((a >> 24) & MD_MASK24); }
static inline uint32_t xc_low(uint64_t a) { return (uint32_t)(a & MD_MASK24); }
static inline uint64_t xc_from24(uint32_t w)
{ return ((uint64_t)(xc_s24(w) * (INT64_C(1) << 24))) & MD_MASK56; }
static inline uint64_t xc_from48(uint64_t w)
{ return (uint64_t)xc_signed_bits(w & MD_MASK48, 48) & MD_MASK56; }

static inline uint64_t xc_setmid(uint64_t a, uint32_t w)
{ return (a & ~(UINT64_C(0xffffff) << 24)) | ((uint64_t)(w & MD_MASK24) << 24); }
static inline uint64_t xc_add(uint64_t a, uint64_t b) { return (a + b) & MD_MASK56; }
static inline uint64_t xc_sub(uint64_t a, uint64_t b) { return (a - b) & MD_MASK56; }

static inline uint64_t xc_asr(uint64_t a, unsigned n)
{
    const int64_t v = xc_s56(a);
    const int64_t sh = v >= 0 ? v / (INT64_C(1) << n)
        : -((-v + (INT64_C(1) << n) - 1) / (INT64_C(1) << n));
    return (uint64_t)sh & MD_MASK56;
}
static inline uint64_t xc_mpy(uint32_t a, uint32_t b)
{ return (uint64_t)(xc_s24(a) * xc_s24(b) * 2) & MD_MASK56; }







static inline uint64_t xc_and_mid(uint64_t a, uint32_t mask)
{ return xc_setmid(a, xc_mid(a) & mask); }
static inline uint64_t xc_pack48(uint32_t hi, uint32_t lo)
{ return ((uint64_t)(hi & MD_MASK24) << 24) | (lo & MD_MASK24); }
static inline uint32_t xc_acc_to_word(uint64_t a)
{
    const int64_t v = xc_s56(a);
    if(v < -(INT64_C(0x800000) << 24)) return UINT32_C(0x800000);
    if(v >  (INT64_C(0x7fffff) << 24)) return UINT32_C(0x7fffff);
    return xc_mid(a);
}

static inline void xc_control_clock_seed(mdkit_control_clock *c, unsigned count,
                                         const unsigned char *knobs)
{
    c->count = count;
    for (unsigned i = 0; i < count; ++i) {
        const float v = (float)knobs[i];
        c->target[i] = c->raw[i] = c->current[i] = c->pending[i] = v;
    }
    c->block = 0; c->next_update = 0; c->publish_block = 0; c->pending_valid = 0;
}


static double xc_local_sin(double x)
{
    x -= 6.283185307179586476925286766559 * (double)(long long)(x / 6.283185307179586476925286766559
                                            + (x < 0.0 ? -0.5 : 0.5));
    if (x > 3.14159265358979323846 / 2.0) x = 3.14159265358979323846 - x;
    else if (x < -3.14159265358979323846 / 2.0) x = -3.14159265358979323846 - x;
    const double x2 = x * x;
    return x * (1.0 - x2 * (1.0 / 6.0 - x2 * (1.0 / 120.0 - x2 * (1.0 / 5040.0
        - x2 * (1.0 / 362880.0 - x2 * (1.0 / 39916800.0))))));
}

static double xc_local_exp2(double x)
{

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
    while(n < 0) { scale *= 0.5; ++n; }
    return sum * scale;
}


static uint32_t xc_local_sine(uint16_t index){double y=8388608.0*xc_local_sin(6.283185307179586476925286766559*(double)(index&32767u)/32768.0);int64_t v=(int64_t)(y+(y<0?-0.5:0.5));if(v>8388607)v=8388607;if(v< -8388608)v=-8388608;return (uint32_t)v&MD_MASK24;}
enum { MDKIT_TRX_PUBLISH=1, MDKIT_TRX_TRIGGER=2, MDKIT_TRX_FIRST=4 };
static inline void xc_trx_events_init(mdkit_control_clock *c,xc_trx_events *e)
{

    xc_control_clock_seed(c,8,mdkit_trx_xc_defaults); c->next_update=2;
    e->targets=e->trigger=e->triggered=0;
    for(unsigned i=0;i<8;++i){e->queued[i]=mdkit_trx_xc_defaults[i];e->last_raw[i]=(unsigned)mdkit_trx_xc_defaults[i] * 128u;}
}
static inline void xc_trx_events_target(xc_trx_events *e,unsigned count,unsigned i,unsigned value)
{ if(i<count){e->queued[i]=value>127?127:value;e->targets|=1u<<i;} }
static inline unsigned xc_trx_events_boundary(mdkit_control_clock *c,xc_trx_events *e,int trigger_only)
{
    unsigned result=0;
    if(c->pending_valid && c->block>=c->publish_block){
        for(unsigned i=0;i<8;++i)c->current[i]=c->pending[i];
        c->pending_valid=0;result=MDKIT_TRX_PUBLISH;
    }
    if(c->block==c->next_update){
        int changed=0;
        for(unsigned i=0;i<8;++i){
            uint32_t raw=(uint32_t)(128.0f*c->raw[i]);
            c->pending[i]=c->raw[i];changed|=raw!=e->last_raw[i];e->last_raw[i]=raw;
            c->raw[i]=(float)((3*raw+128*(uint32_t)c->target[i])/4)/128.0f;
        }
        c->pending_valid=changed&&!trigger_only;c->publish_block=c->block+1;c->next_update+=12;
    }
    for(unsigned i=0;i<8;++i)if(e->targets&(1u<<i))c->target[i]=(float)e->queued[i];
    e->targets=0;
    if(e->trigger){
        for(unsigned i=0;i<8;++i)c->current[i]=c->raw[i];
        result=MDKIT_TRX_PUBLISH|MDKIT_TRX_TRIGGER|(e->triggered?0:MDKIT_TRX_FIRST);
        e->trigger=0;e->triggered=1;
    }
    ++c->block;return result;
}
static uint32_t xc_decay_curve(unsigned i){ const double x=(double)i/159.0; static const double c[6]={14.46804079054191,-5.381659271041384,0.0033378268939893894,-0.034151947423199602,0.051297926251158636,-0.024358971177662701}; double y=c[5]; for(unsigned j=5;j>0;--j)y=y*x+c[j-1]; y=8388608.0-xc_local_exp2(y); return (uint32_t)(y+0.5)&MD_MASK24;}
static uint32_t xc_pitch_curve(unsigned i){ const double x=(double)i/255.0; static const double c[9]={4093.2624326224718,5871.2047403592287,824.50058251442022,20991.894089762365,-52806.614842877345,68646.933736058316,-32114.933496491751,-6392.2013632470544,7229.0531236803909}; double y=c[8]; for(unsigned j=8;j>0;--j)y=y*x+c[j-1]; return (uint32_t)(y+0.5)&MD_MASK24;}
static void trx_xc_packet(const float k[8],uint32_t o[9]){unsigned r[8];for(unsigned j=0;j<8;++j)r[j]=(unsigned)(128.0f*k[j]);o[0]=xc_decay_curve((r[1]*160u)>>14);o[1]=xc_pitch_curve(r[0]>>6);o[2]=(r[2]+3u)>>6;o[3]=(8388607u-384u*r[5])&MD_MASK24;o[4]=(8388609u+384u*r[5])&MD_MASK24;o[5]=(r[6]*r[6]>>14)*2u;o[6]=8134709u+15u*r[3];o[7]=8388607u-256u*r[4];o[8]=4194304u+256u*r[4];}
static uint32_t xc_attack_shape(unsigned i){double y=0.0;
if(i>=0u && i<=3u){const double x=2.0*(double)(i-0u)/3.0-1.0; static const double c[3]={-0.0025691986083984531,0.026545715332031258,0.034246444702148458};y=c[2];for(unsigned j=2;j>0;--j)y=y*x+c[j-1];}
if(i>=4u && i<=9u){const double x=2.0*(double)(i-4u)/5.0-1.0; static const double c[5]={0.8468953371047967,0.27867324768550805,-0.20030140876770039,0.041948424445257947,0.0090152025222783472};y=c[4];for(unsigned j=4;j>0;--j)y=y*x+c[j-1];}
if(i>=10u && i<=31u){const double x=2.0*(double)(i-10u)/21.0-1.0; static const double c[6]={0.62225154896716139,-0.4870805074044991,-0.061070147615355536,0.026918897801077934,-0.026485142834720355,0.01733886241493077};y=c[5];for(unsigned j=5;j>0;--j)y=y*x+c[j-1];}
int64_t w=(int64_t)(y*8388608.0+(y<0?-0.5:0.5));return (uint32_t)w&MD_MASK24;}




static uint32_t xc_sine(uint16_t index){return xc_local_sine(index);}

static void xc_build_controls(mdkit_trx_xc *s, int full)
{
    uint32_t packet[9];trx_xc_packet(s->clock.current,packet);
    s->decay=packet[0];s->pitch=packet[1];s->ramp_amount=packet[2];
    s->fold_upper=packet[3];s->fold_lower=packet[4];s->fold_step=packet[5];s->ramp_decay=packet[6];
    if(full){s->damp_seed=packet[7];s->amplitude=packet[7];s->attack_gain=packet[8];}
}

static void xc_trx_xc_init(void *state)
{
    mdkit_trx_xc *s = (mdkit_trx_xc *)state;
    xc_trx_events_init(&s->clock,&s->events);
    s->pos = 0;
    s->decay = s->pitch = s->ramp_amount = s->fold_upper = s->fold_lower = s->fold_step = s->ramp_decay = 0;
    s->amplitude = s->attack_gain = s->ramp = s->stage = 0;
    s->phase_hi = s->phase_lo = 0;
    s->out_prev = s->dmp_a = s->dmp_b = 0;
    s->damp_seed = 0;
    for (unsigned i = 0; i < 32; ++i) s->delay[i] = 0;
    s->have_block = 0;
    xc_build_controls(s,1);
}

static void xc_trx_xc_set_control(void *state, unsigned i, unsigned v)
{
    xc_trx_events_target(&((mdkit_trx_xc *)state)->events,7,i,v);
}


static void xc_apply_trigger(void *state)
{
    mdkit_trx_xc *s = (mdkit_trx_xc *)state;
    s->stage = 3;
    s->ramp  = 0x7ffcb9u;
    s->phase_hi = 0; s->phase_lo = 0;
    s->amplitude = s->damp_seed;
    s->dmp_a = s->fold_upper;
    s->dmp_b = s->fold_lower;
    s->have_block = 0;
}
static void xc_trx_xc_trigger(void *state)
{ ((mdkit_trx_xc *)state)->events.trigger=1; }



static void xc_ring32(mdkit_trx_xc *s, uint32_t out[32])
{
    uint32_t x1 = s->out_prev;
    for (unsigned i = 0; i < 32; ++i) {
        const uint32_t a = xc_mid(xc_mpy(x1, 0x733333u));
        out[i] = a;
        x1 = a;
    }
}


static void xc_silence32(mdkit_trx_xc *s, uint32_t out[32])
{
    (void)s;
    for (unsigned i = 0; i < 32; ++i) out[i] = 0;
}


static void xc_attack_window(mdkit_trx_xc *s, uint32_t out[32])
{
    const uint32_t scale = s->attack_gain;
    for (unsigned i = 0; i < 32; ++i) {
        out[i] = xc_mid(xc_mpy(scale, xc_attack_shape(i)));
    }
}


static void xc_main(mdkit_trx_xc *s, uint32_t out[32])
{
    uint32_t ring[32];


    uint64_t a = xc_asr(xc_from24(s->pitch), 6);
    const uint32_t ring_sum = s->ramp;
    a = xc_add(a, xc_mpy(ring_sum, s->ramp_amount));
    s->ramp = xc_mid(xc_mpy(ring_sum, s->ramp_decay));


    uint32_t x1 = xc_mid(a);
    uint32_t x0 = xc_low(a);
    uint64_t phase = xc_from48(xc_pack48(s->phase_hi, s->phase_lo));
    for (unsigned i = 0; i < 32; ++i) {
        const uint16_t index = (uint16_t)(xc_mid(phase) & 0x7fffu);
        phase = xc_and_mid(phase,0x7fff);
        phase = xc_add(phase, xc_from48(xc_pack48(x1, x0)));
        x0 = xc_sine(index);
        ring[i] = x0;
    }
    s->phase_hi = xc_mid(phase);
    s->phase_lo = xc_low(phase);


    {
        uint32_t damping = s->amplitude;
        for (unsigned i = 0; i < 32; ++i) {
            const uint32_t filtered = xc_mid(xc_mpy(damping, ring[i]));
            damping = xc_mid(xc_mpy(damping, s->decay));
            ring[i] = filtered;
        }
        s->amplitude = damping;
    }


    {
        uint64_t lo = xc_from24(s->dmp_a);
        uint64_t hi = xc_from24(s->dmp_b);
        const uint64_t dist_a = xc_from24(s->fold_upper);
        const uint64_t dist_b = xc_from24(s->fold_lower);
        const uint64_t step_dt = xc_from24(s->fold_step);
        for (unsigned i = 0; i < 32; ++i) {
            uint64_t b = xc_sub(0, xc_from24(ring[i]));
            if (xc_s56(b) < xc_s56(dist_a)) {
                if (xc_s56(b) > xc_s56(dist_b)) {

                } else {
                    b = xc_add(hi, step_dt);
                    lo = dist_a;
                    hi = xc_from24(xc_acc_to_word(b));
                }
            } else {
                b = xc_sub(lo, step_dt);
                hi = dist_b;
                lo = xc_from24(xc_acc_to_word(b));
            }
            out[i] = xc_acc_to_word(b);
        }
        s->dmp_a = xc_mid(lo);
        s->dmp_b = xc_mid(hi);
    }
    s->out_prev = out[31];
}

static void xc_trx_xc_render(void *state, float *out, unsigned frames)
{
    mdkit_trx_xc *s = (mdkit_trx_xc *)state;
    while (frames) {
        if (s->pos == 0u) {
            unsigned event=xc_trx_events_boundary(&s->clock,&s->events,0);
            if(event&MDKIT_TRX_PUBLISH)xc_build_controls(s,(event&MDKIT_TRX_TRIGGER)!=0);
            if(event&MDKIT_TRX_TRIGGER)xc_apply_trigger(s);
                    s->have_block = 0;
        }
        if (!s->events.triggered) {
            for(unsigned i=0;i<32;++i)s->block[i]=0;
            s->have_block=1;
        }
        if (!s->have_block) {

            const uint32_t st = s->stage;
            if (st == 0u) {
                xc_main(s, s->block);
            } else {
                const uint32_t a = st - 1u;
                s->stage = a;
                if (a == 0u)             xc_attack_window(s, s->block);
                else if (st - 2u == 0u)  xc_silence32(s, s->block);
                else                     xc_ring32(s, s->block);
            }
            s->have_block = 1;
        }
        unsigned n = 32u - s->pos;
        if (n > frames) n = frames;
        for (unsigned i = 0; i < n; ++i) {
            const uint32_t w = s->block[s->pos + i] & MD_MASK24;
            out[i] = (float)(int32_t)((w & 0x800000u) ? (int32_t)w - 0x1000000 : (int32_t)w) / 8388608.0f;
        }
        out += n; frames -= n; s->pos += n;
        if (s->pos >= 32u) s->pos = 0;
    }
}

static const char *const xc_trx_xc_control_names[MDKIT_TRX_XC_CONTROLS] = {
    "PTCH", "DEC", "RAMP", "RDEC", "DAMP", "DIST", "DTYP"
};

const mdkit_voice mdkit_voice_trx_xc = {
    "TRX-XC", 27u, MDKIT_TRX_XC_CONTROLS, xc_trx_xc_control_names,
    sizeof(mdkit_trx_xc),
    xc_trx_xc_init, xc_trx_xc_set_control, xc_trx_xc_trigger, xc_trx_xc_render, mdkit_trx_xc_defaults
};
