/* SPDX-License-Identifier: CC0-1.0
 * mdkit_efm_bd.c - EFM-BD standalone voice
 */
#include <stdint.h>
#include <stddef.h>
#include "mdkit_efm_bd.h"

#define MDKIT_TWO_PI 6.283185307179586476925286766559
#define MDKIT_PI     3.14159265358979323846
static __attribute__((unused)) double mdkit_exp2(double x)
{
    long long n=(long long)x; double f=x-(double)n;
    if(f<0.0){f+=1.0;--n;}
    const double frac=1.0+f*(0.6931471805599453
        +f*(0.2402265069591007+f*(0.05550410866482158
        +f*0.009618129107628477)));
    double scale=frac;
    while(n>0){scale*=2.0;--n;} while(n<0){scale*=0.5;++n;}
    return scale;
}
static __attribute__((unused)) double mdkit_sin(double x)
{
    x-=MDKIT_TWO_PI*(double)(long long)(x/MDKIT_TWO_PI+(x<0.0?-0.5:0.5));
    if(x> MDKIT_PI/2.0)x=MDKIT_PI-x; else if(x<-MDKIT_PI/2.0)x=-MDKIT_PI-x;
    const double z=x*x;
    return x*(1.0-z*(1.0/6.0-z*(1.0/120.0-z*(1.0/5040.0-z*(1.0/362880.0-z*(1.0/39916800.0))))));
}





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



typedef struct { double ptch, dec, ramp, rdec, mod, mfrq, mdec, mfb; } bd_coeff;

static bd_coeff mdkit_efm_bd_coeff(const float k[MDKIT_EFM_BD_CONTROLS])
{
    bd_coeff c;
    c.ptch = 0.000581 * mdkit_exp2((double)k[MDKIT_EFM_BD_PTCH] / 32.0);
    c.dec  = 1.0 - 0.009469 * mdkit_exp2(-(double)k[MDKIT_EFM_BD_DEC]  / 13.5);
    c.rdec = 1.0 - 0.009468 * mdkit_exp2(-(double)k[MDKIT_EFM_BD_RDEC] / 12.6);
    c.mdec = 1.0 - 0.009474 * mdkit_exp2(-(double)k[MDKIT_EFM_BD_MDEC] / 11.52);
    c.ramp = ((double)k[MDKIT_EFM_BD_RAMP] - 16.0) / 128.0;   /* signed, exact */
    c.mod  =  (double)k[MDKIT_EFM_BD_MOD]  / 128.0;           /* exact         */
    c.mfb  =  (double)k[MDKIT_EFM_BD_MFB]  / 128.0;           /* exact         */
    const double mfrq = (double)k[MDKIT_EFM_BD_MFRQ];
    c.mfrq = (mfrq < 1.0 ? 1.0 : mfrq) / 128.0;            /* max(1,v)/128  */
    return c;
}

/* ---- Default controls --------------------------------------------------- */

static const unsigned char mdkit_efm_bd_defaults[MDKIT_EFM_BD_CONTROLS] = {
    0, 64, 32, 127, 35, 14, 64, 24
};

static void mdkit_efm_bd_init(void *state)
{
    mdkit_efm_bd *s = (mdkit_efm_bd *)state;
    efm_clock_seed(&s->clock, MDKIT_EFM_BD_CONTROLS, mdkit_efm_bd_defaults);
    s->pos = 0;
    s->ph1 = s->ph2 = s->mod = 0.0;
    s->op1 = s->op2 = s->fb = 0.0;
}

static void mdkit_efm_bd_set_control(void *state, unsigned i, unsigned v)
{
    efm_clock_target(&((mdkit_efm_bd *)state)->clock, i, v);
}

static void mdkit_efm_bd_trigger(void *state)
{
    mdkit_efm_bd *s = (mdkit_efm_bd *)state;
    const bd_coeff c = mdkit_efm_bd_coeff(s->clock.current);
    s->op1 = c.ramp;                 /* attack knock, from RAMP            */
    s->op2 = 1.0;                    /* full scale                         */
    s->fb  = 0.630957 + s->fb;       /* 0x50c335/2^23, ADDS retained value */
    s->ph1 = s->ph2 = 4000.0;        /* seed 0x000fa0, index units         */
    s->mod = 0.0;
}

static void mdkit_efm_bd_render(void *state, float *out, unsigned frames)
{
    mdkit_efm_bd *s = (mdkit_efm_bd *)state;
    while (frames) {
        if (s->pos == 0u) efm_clock_boundary(&s->clock);
        const bd_coeff c = mdkit_efm_bd_coeff(s->clock.current);
        unsigned n = 32u - s->pos;
        if (n > frames) n = frames;
        for (unsigned i = 0; i < n; ++i) {
            const double op1_prev = s->op1;
            s->op1 = c.rdec * op1_prev;
            const double y1 = c.ptch * (1.0 + 8.0 * op1_prev);

            double i1 = s->ph1 + s->mod;
            i1 -= 32768.0 * (double)(long long)(i1 / 32768.0);
            if (i1 < 0.0) i1 += 32768.0;
            const double s1 = mdkit_sin(MDKIT_TWO_PI * i1 / 32768.0);

            s->ph1 += 262144.0 * c.mfrq * y1;
            s->mod  = 32768.0 * c.mfb * s1;

            const double op2_prev = s->op2;
            s->op2 = c.mdec * op2_prev;
            const double x1c = op2_prev * s1 * y1;

            double i2 = s->ph2;
            i2 -= 32768.0 * (double)(long long)(i2 / 32768.0);
            if (i2 < 0.0) i2 += 32768.0;
            const double s2 = mdkit_sin(MDKIT_TWO_PI * i2 / 32768.0);

            s->ph2 += 32768.0 * (8.0 * c.mod * x1c + y1);

            const double o = s->fb * s2;
            s->fb = c.dec * s->fb;
            out[i] = (float)o;
        }
        out += n; frames -= n; s->pos += n;
        if (s->pos >= 32u) s->pos = 0;
    }
}

static const char *const mdkit_efm_bd_control_names[MDKIT_EFM_BD_CONTROLS] = {
    "PTCH", "DEC", "RAMP", "RDEC", "MOD", "MFRQ", "MDEC", "MFB"
};

const mdkit_voice mdkit_voice_efm_bd = {
    "EFM-BD", 32u, MDKIT_EFM_BD_CONTROLS, mdkit_efm_bd_control_names,
    sizeof(mdkit_efm_bd),
    mdkit_efm_bd_init, mdkit_efm_bd_set_control, mdkit_efm_bd_trigger, mdkit_efm_bd_render, mdkit_efm_bd_defaults
};
