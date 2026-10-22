/* SPDX-License-Identifier: CC0-1.0
 * TRX-CL standalone synthesis voice.
 */
#include "mdkit_trx_cl.h"

/* ---- Default controls --------------------------------------------------- */
/* Active controls are in descriptor order; unused clock slots stay at 64. */
static const unsigned char mdkit_trx_cl_defaults[MDKIT_MAX_CONTROLS] = {
    64, 32, 64, 0, 64, 0, 64, 64
};
#define trx_cl_MD_MASK24 UINT32_C(0xffffff)
#define trx_cl_MD_MASK48 UINT64_C(0xffffffffffff)
#define trx_cl_MD_MASK56 UINT64_C(0xffffffffffffff)
#define trx_cl_MD_VOICE_BLOCK 32u
enum { trx_cl_MD_TRX_PUBLISH=1,trx_cl_MD_TRX_TRIGGER=2,trx_cl_MD_TRX_FIRST=4 };
static inline int64_t trx_cl_md_signed_bits(uint64_t word, unsigned bits)
{
    const uint64_t sign = UINT64_C(1) << (bits - 1);
    const uint64_t mask = (UINT64_C(1) << bits) - 1;
    word &= mask;
    return (word & sign) ? (int64_t)word - (int64_t)(UINT64_C(1) << bits)
                         : (int64_t)word;
}
static inline int64_t trx_cl_md_s24(uint32_t w) { return trx_cl_md_signed_bits(w & trx_cl_MD_MASK24, 24); }
static inline int64_t trx_cl_md_s56(uint64_t a) { return trx_cl_md_signed_bits(a & trx_cl_MD_MASK56, 56); }
static inline uint32_t trx_cl_md_mid(uint64_t a) { return (uint32_t)((a >> 24) & trx_cl_MD_MASK24); }
static inline uint32_t trx_cl_md_low(uint64_t a) { return (uint32_t)(a & trx_cl_MD_MASK24); }
static inline uint64_t trx_cl_md_from24(uint32_t w)
{ return ((uint64_t)(trx_cl_md_s24(w) * (INT64_C(1) << 24))) & trx_cl_MD_MASK56; }
static inline uint64_t trx_cl_md_from48(uint64_t w)
{ return (uint64_t)trx_cl_md_signed_bits(w & trx_cl_MD_MASK48, 48) & trx_cl_MD_MASK56; }
static inline uint64_t trx_cl_md_setmid(uint64_t a, uint32_t w)
{ return (a & ~(UINT64_C(0xffffff) << 24)) | ((uint64_t)(w & trx_cl_MD_MASK24) << 24); }
static inline uint64_t trx_cl_md_add(uint64_t a, uint64_t b) { return (a + b) & trx_cl_MD_MASK56; }
static inline uint64_t trx_cl_md_sub(uint64_t a, uint64_t b) { return (a - b) & trx_cl_MD_MASK56; }
static inline uint64_t trx_cl_md_asl(uint64_t a, unsigned n) { return (a << n) & trx_cl_MD_MASK56; }
static inline uint64_t trx_cl_md_asr(uint64_t a, unsigned n)
{
    const int64_t v = trx_cl_md_s56(a);
    const int64_t sh = v >= 0 ? v / (INT64_C(1) << n)
        : -((-v + (INT64_C(1) << n) - 1) / (INT64_C(1) << n));
    return (uint64_t)sh & trx_cl_MD_MASK56;
}
static inline uint64_t trx_cl_md_mpy(uint32_t a, uint32_t b)
{ return (uint64_t)(trx_cl_md_s24(a) * trx_cl_md_s24(b) * 2) & trx_cl_MD_MASK56; }
static inline uint64_t trx_cl_md_mac(uint64_t acc, uint32_t x, uint32_t y)
{
    return (acc + trx_cl_md_mpy(x, y)) & trx_cl_MD_MASK56;
}
static inline uint64_t trx_cl_md_and_mid(uint64_t a, uint32_t mask)
{ return trx_cl_md_setmid(a, trx_cl_md_mid(a) & mask); }
static inline uint64_t trx_cl_md_pack48(uint32_t hi, uint32_t lo)
{ return ((uint64_t)(hi & trx_cl_MD_MASK24) << 24) | (lo & trx_cl_MD_MASK24); }
static inline uint32_t trx_cl_md_acc_to_word(uint64_t a)
{
    const int64_t v = trx_cl_md_s56(a);
    if(v < -(INT64_C(0x800000) << 24)) return UINT32_C(0x800000);
    if(v >  (INT64_C(0x7fffff) << 24)) return UINT32_C(0x7fffff);
    return trx_cl_md_mid(a);
}
static inline void trx_cl_md_control_clock_seed(mdkit_control_clock *c, unsigned count,
                                         const unsigned char *knobs)
{
    c->count = count;
    for (unsigned i = 0; i < count; ++i) {
        const float v = (float)knobs[i];
        c->target[i] = c->raw[i] = c->current[i] = c->pending[i] = v;
    }
    c->block = 0; c->next_update = 0; c->publish_block = 0; c->pending_valid = 0;
}
static inline void trx_cl_md_trx_events_init(mdkit_control_clock *c,mdkit_trx_cl_events *e)
{

    trx_cl_md_control_clock_seed(c,8,mdkit_trx_cl_defaults); c->next_update=2;
    e->targets=e->trigger=e->triggered=0;
    for(unsigned i=0;i<8;++i){e->queued[i]=mdkit_trx_cl_defaults[i];e->last_raw[i]=(unsigned)mdkit_trx_cl_defaults[i] * 128u;}
}
static inline void trx_cl_md_trx_events_target(mdkit_trx_cl_events *e,unsigned count,unsigned i,unsigned value)
{ if(i<count){e->queued[i]=value>127?127:value;e->targets|=1u<<i;} }
static inline unsigned trx_cl_md_trx_events_boundary(mdkit_control_clock *c,mdkit_trx_cl_events *e,int trigger_only)
{
    unsigned result=0;
    if(c->pending_valid && c->block>=c->publish_block){
        for(unsigned i=0;i<8;++i)c->current[i]=c->pending[i];
        c->pending_valid=0;result=trx_cl_MD_TRX_PUBLISH;
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
        result=trx_cl_MD_TRX_PUBLISH|trx_cl_MD_TRX_TRIGGER|(e->triggered?0:trx_cl_MD_TRX_FIRST);
        e->trigger=0;e->triggered=1;
    }
    ++c->block;return result;
}
static uint32_t trx_cl_local_sine(unsigned index) {
 const double pi=3.1415926535897932384626433832795;
 double x=(double)(index&32767u)*(2.0*pi/32768.0);
 if(x>pi)x-=2.0*pi;
 if(x>pi/2.0)x=pi-x;else if(x< -pi/2.0)x=-pi-x;
 double z=x*x;
 double y=x*(1.0-z*(1.0/6.0-z*(1.0/120.0-z*(1.0/5040.0-z*(1.0/362880.0-z*(1.0/39916800.0-z/6227020800.0))))));
 double a=y*8388608.0;
 int64_t q=(int64_t)(a<0?a-0.5:a+0.5);
 if(q>8388607)q=8388607;if(q< -8388608)q=-8388608;
 return (uint32_t)q&0xffffffu;
}

static inline double trx_cl_trx_model_exp(double);
static inline double trx_cl_trx_model_cheb(const double*,unsigned,double);
static const double trx_cl_cl_decay_coeff[7]={8.1623740654127861,-1.8664496048356374,-0.00033746655155518951,-1.4231472247634541e-06,-6.3656175524435686e-05,-3.2977250378611249e-05,-2.4376420967052329e-05};
static void trx_cl_trx_cl_packet(const float k[8],uint32_t p[8]){
 uint32_t raw[8];for(unsigned i=0;i<8;++i)raw[i]=(uint32_t)(128*k[i]);
 unsigned index=(raw[1]*5)>>9;
 double x=2.0*index/159.0-1.0;
 p[0]=(uint32_t)(8388608.0-trx_cl_trx_model_exp(trx_cl_trx_model_cheb(trx_cl_cl_decay_coeff,7,x))+0.5);
 p[1]=15u+((raw[2]+20u)>>5);
 p[2]=1400u+raw[0]/5u;p[3]=1400u+raw[4]/5u;
 p[4]=1112u+raw[0]/41u;p[5]=64u+(raw[3]>>8);
 p[6]=1048576u+raw[4]*256u;p[7]=((uint32_t)(-512)*(raw[5]))&0xffffffu;
}



#ifndef TRX_COMPACT_MODEL_MATH
#define TRX_COMPACT_MODEL_MATH
static inline double trx_cl_trx_model_exp(double x) {
    const double ln2=0.69314718055994530942;
    int n=(int)(x/ln2+(x<0?-0.5:0.5));
    double r=x-n*ln2,term=1,sum=1;
    for(unsigned k=1;k<=18;++k) { term*=r/k;sum+=term; }
    while(n>0) {sum*=2;--n;}
    while(n<0) {sum*=0.5;++n;}
    return sum;
}
static inline double trx_cl_trx_model_cos(double x) {
    double term=1,sum=1;
    for(unsigned k=1;k<=18;++k) {term*=-x*x/((2*k-1)*(2*k));sum+=term;}
    return sum;
}
static inline long long trx_model_round(double x) {return (long long)(x+(x<0?-0.5:0.5));}
static inline uint32_t trx_cl_trx_model_word(double v) {
    if (v > 8388607.0) v = 8388607.0;
    if (v < -8388608.0) v = -8388608.0;
    return (uint32_t)(int32_t)trx_model_round(v) & 0xffffffu;
}
static inline double trx_cl_trx_model_cheb(const double *c,unsigned n,double x) {
    double b1=0,b2=0;
    for(unsigned k=n;k>1;--k) { double b=2*x*b1-b2+c[k-1]; b2=b1;b1=b; }
    return x*b1-b2+c[0];
}
#endif
static const double trx_cl_trx_model_offset_a_c[30]={-232.399680398,153.649305252,-56.7606534091,23.5448649658,-10.6578480114,4.79767127404,-81.056751598,22.7077837039,-2.96244673295,0.299532376803,0.0532892400568,-0.0105212089343,-39.1668372957,14.9894597998,-2.21767366486,0.405478971463,-0.0117738691504,0.138342962517,-15.9882689259,7.39288842873,-1.35951208431,0.190096146761,0.00637058241878,-0.116429618127,-5.5138140884,3.0706297243,-0.547032748071,0.15553066531,0.0144368154093,-0.0443455945788};
static const double trx_cl_trx_model_offset_b_c[13]={-357.643862343,-265.373321533,59.3405978631,-17.7956076723,5.97881763422,-2.11982610249,0.784178870543,-0.267558388544,0.0889657720231,0.0650633677486,0.00493977326511,-0.0123225741715,-0.021290158449};
static inline int trx_model_offset(unsigned i) {
    if(i>=128) return (int)trx_model_round(trx_cl_trx_model_cheb(trx_cl_trx_model_offset_b_c,13,2.0*(i-128)/127.0-1.0));
    unsigned part=i<8?0:i<16?1:i<32?2:i<64?3:4;
    static const unsigned trx_cl_bounds[]={0,8,16,32,64,128};
    unsigned lo=trx_cl_bounds[part],hi=trx_cl_bounds[part+1];
    return (int)trx_model_round(trx_cl_trx_model_cheb(trx_cl_trx_model_offset_a_c+part*6,6,2.0*(i-lo)/(hi-lo-1)-1.0));
}
static inline uint32_t trx_cl_trx_model_memory(unsigned i) {
    if(i<256) return (uint32_t)trx_model_offset(i)&0xffffffu;
    unsigned x=i<4352?i-256:i-4352;
    double t=trx_cl_trx_model_exp(0.003372927270490704*x+(-12.768360569597025));
    return trx_cl_trx_model_word(5931641.6632877039*(i<4352?trx_cl_trx_model_cos(t):trx_cl_trx_model_exp(-t)));
}

static uint32_t trx_cl_trx_filter_memory(unsigned i){return trx_cl_trx_model_memory(i);}






static void trx_cl_cl_controls(mdkit_trx_cl *s)
{
    uint32_t p[8];trx_cl_trx_cl_packet(s->clock.current,p);
    s->decay=p[0];s->fill_count=p[1];s->pitch_a=p[2];s->pitch_b=p[3];
    s->filter_offset=p[4];s->filter_shape=p[5];s->dual_gain=p[6];s->click=p[7];
}
static void trx_cl_cl_init(void *state)
{
    mdkit_trx_cl*s=state;trx_cl_md_trx_events_init(&s->clock,&s->events);s->pos=0;
    s->amplitude=s->phase_a_hi=s->phase_a_lo=s->phase_b_hi=s->phase_b_lo=0;
    s->input_a=s->input_b=s->output_a=s->output_b=s->delay_a=s->delay_b=0;
    for(unsigned i=0;i<128;++i)s->workspace[i]=0;
    for(unsigned i=0;i<32;++i)s->block[i]=0;
    trx_cl_cl_controls(s);
}
static void trx_cl_cl_target(void*state,unsigned i,unsigned value)
{trx_cl_md_trx_events_target(&((mdkit_trx_cl*)state)->events,6,i,value);}
static void trx_cl_cl_trigger(void*state){((mdkit_trx_cl*)state)->events.trigger=1;}
static void trx_cl_cl_scan(uint32_t *hi,uint32_t *lo,uint64_t increment,uint32_t *w,unsigned start,unsigned count)
{
    uint64_t phase=trx_cl_md_from48(trx_cl_md_pack48(*hi,*lo));
    uint32_t step_hi=trx_cl_md_mid(increment),step_lo=trx_cl_md_low(increment);
    for(unsigned i=0;i<count;++i){
        phase=trx_cl_md_and_mid(phase,0x7fff);unsigned index=trx_cl_md_mid(phase);
        phase=trx_cl_md_add(phase,trx_cl_md_from48(trx_cl_md_pack48(step_hi,step_lo)));
        step_lo=trx_cl_local_sine(index);w[(start+i)&31]=step_lo;
    }
    *hi=trx_cl_md_mid(phase);*lo=trx_cl_md_low(phase);
}
static void trx_cl_cl_filter(mdkit_trx_cl*s)
{
    uint32_t*w=s->workspace,shape=s->filter_shape;
    uint32_t norm_index=(s->filter_offset+trx_cl_trx_filter_memory(shape))&trx_cl_MD_MASK24;
    uint32_t coef_index=(s->filter_offset+trx_cl_trx_filter_memory(0x80+shape))&trx_cl_MD_MASK24;
    uint32_t resonance=trx_cl_trx_filter_memory(0x1900+coef_index);
    uint32_t feedback=trx_cl_md_acc_to_word(trx_cl_md_asl(trx_cl_md_mpy(resonance,trx_cl_trx_filter_memory(0x900+norm_index)),1));
    uint32_t square=trx_cl_md_acc_to_word(trx_cl_md_sub(0,trx_cl_md_mpy(resonance,resonance)));
    const uint32_t half=0x400000,negative_one=0x800000;
    w[0]=s->input_b;w[1]=s->input_a;w[64]=s->output_b;w[65]=s->output_a;
    w[30]=s->delay_b;w[31]=s->delay_a;
    unsigned history=1,input=31,output=65;
    uint32_t source=w[30];
    uint64_t first=trx_cl_md_mac(trx_cl_md_mpy(square,w[0]),feedback,w[1]);
    for(unsigned i=0;i<32;++i){
        first=trx_cl_md_mac(first,source,half);
        first=trx_cl_md_mac(first,w[input++],negative_one);
        first=trx_cl_md_mac(first,w[input--],half);
        --output;
        uint32_t previous=w[output++];
        first=trx_cl_md_asl(first,1);
        uint64_t second=trx_cl_md_mpy(square,previous);
        second=trx_cl_md_mac(second,feedback,w[output++]);
        uint32_t first_word=trx_cl_md_acc_to_word(first);
        second=trx_cl_md_mac(second,first_word,half);
        second=trx_cl_md_mac(second,w[history],negative_one);history^=1;
        second=trx_cl_md_mac(second,w[history],half);
        w[history]=first_word;history^=1;
        second=trx_cl_md_asl(second,1);
        first=trx_cl_md_mpy(square,w[history]);history^=1;
        source=w[input++];
        first=trx_cl_md_mac(first,first_word,feedback);
        w[output]=trx_cl_md_acc_to_word(second);
    }
    s->input_a=w[history];history^=1;s->input_b=w[history];
    s->output_a=w[output--];s->output_b=w[output];
    s->delay_a=w[input];s->delay_b=w[input-1];
}
static void trx_cl_cl_block(mdkit_trx_cl*s)
{
    uint32_t*w=s->workspace;
    if(!s->fill_count)trx_cl_cl_scan(&s->phase_a_hi,&s->phase_a_lo,trx_cl_md_asr(trx_cl_md_from24(s->pitch_a),1),w,0,32);
    else if(s->fill_count>32){for(unsigned i=0;i<32;++i)w[i]=s->click;s->fill_count-=32;}
    else{
        unsigned fill=s->fill_count;for(unsigned i=0;i<fill;++i)w[i]=s->click;
        unsigned remaining=32-fill;
        trx_cl_cl_scan(&s->phase_a_hi,&s->phase_a_lo,trx_cl_md_from24(remaining),w,fill,remaining?remaining:65536);
        s->fill_count=0;
    }
    trx_cl_cl_scan(&s->phase_b_hi,&s->phase_b_lo,trx_cl_md_asr(trx_cl_md_from24(s->pitch_b),1),w+32,0,32);
    for(unsigned i=0;i<32;++i)w[32+i]=trx_cl_md_acc_to_word(trx_cl_md_mpy(s->dual_gain,w[32+i]));
    for(unsigned i=0;i<32;++i){
        uint32_t mixed=trx_cl_md_acc_to_word(trx_cl_md_add(trx_cl_md_from24(w[i]),trx_cl_md_from24(w[32+i])));
        w[32+i]=trx_cl_md_acc_to_word(trx_cl_md_asr(trx_cl_md_mpy(s->amplitude,mixed),3));
        s->amplitude=trx_cl_md_acc_to_word(trx_cl_md_mpy(s->amplitude,s->decay));
    }
    trx_cl_cl_filter(s);for(unsigned i=0;i<32;++i)s->block[i]=w[64+i];
}
static void trx_cl_cl_render(void*state,float*out,unsigned frames)
{
    mdkit_trx_cl*s=state;
    while(frames){
        if(!s->pos){unsigned event=trx_cl_md_trx_events_boundary(&s->clock,&s->events,1);
            if(event&trx_cl_MD_TRX_PUBLISH)trx_cl_cl_controls(s);
            if(event&trx_cl_MD_TRX_FIRST){s->amplitude=0;s->input_a=s->input_b=s->output_a=s->output_b=s->delay_a=s->delay_b=0;}
            if(event&trx_cl_MD_TRX_TRIGGER){s->amplitude=trx_cl_md_acc_to_word(trx_cl_md_add(trx_cl_md_from24(0x50c335),trx_cl_md_from24(s->amplitude)));s->phase_a_hi=s->phase_a_lo=s->phase_b_hi=s->phase_b_lo=0;}
            trx_cl_cl_block(s);
        }
        unsigned n=32-s->pos;if(n>frames)n=frames;
        for(unsigned i=0;i<n;++i)out[i]=(float)trx_cl_md_s24(s->block[s->pos+i])/8388608.0f;
        out+=n;frames-=n;s->pos=(s->pos+n)&31;
    }
}
static const char*const trx_cl_names[]={"PTCH","DEC","DUAL","ENH","TUNE","CLIC"};
const mdkit_voice mdkit_voice_trx_cl={"TRX-CL",26,6,trx_cl_names,sizeof(mdkit_trx_cl),trx_cl_cl_init,trx_cl_cl_target,trx_cl_cl_trigger,trx_cl_cl_render, mdkit_trx_cl_defaults
};
