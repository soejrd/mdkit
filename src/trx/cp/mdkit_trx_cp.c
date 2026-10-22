/* SPDX-License-Identifier: CC0-1.0
 * TRX-CP standalone synthesis voice.
 */
#include "mdkit_trx_cp.h"

/* ---- Default controls --------------------------------------------------- */
/* Active controls are in descriptor order; unused clock slots stay at 64. */
static const unsigned char mdkit_trx_cp_defaults[MDKIT_MAX_CONTROLS] = {
    50, 100, 7, 127, 40, 64, 0, 90
};
#define trx_cp_MD_MASK24 UINT32_C(0xffffff)
#define trx_cp_MD_MASK48 UINT64_C(0xffffffffffff)
#define trx_cp_MD_MASK56 UINT64_C(0xffffffffffffff)
#define trx_cp_MD_VOICE_BLOCK 32u
enum { trx_cp_MD_TRX_PUBLISH=1,trx_cp_MD_TRX_TRIGGER=2,trx_cp_MD_TRX_FIRST=4 };
static inline int64_t trx_cp_md_signed_bits(uint64_t word, unsigned bits)
{
    const uint64_t sign = UINT64_C(1) << (bits - 1);
    const uint64_t mask = (UINT64_C(1) << bits) - 1;
    word &= mask;
    return (word & sign) ? (int64_t)word - (int64_t)(UINT64_C(1) << bits)
                         : (int64_t)word;
}
static inline int64_t trx_cp_md_s24(uint32_t w) { return trx_cp_md_signed_bits(w & trx_cp_MD_MASK24, 24); }
static inline int64_t trx_cp_md_s56(uint64_t a) { return trx_cp_md_signed_bits(a & trx_cp_MD_MASK56, 56); }
static inline uint32_t trx_cp_md_mid(uint64_t a) { return (uint32_t)((a >> 24) & trx_cp_MD_MASK24); }
static inline uint64_t trx_cp_md_from24(uint32_t w)
{ return ((uint64_t)(trx_cp_md_s24(w) * (INT64_C(1) << 24))) & trx_cp_MD_MASK56; }
static inline uint64_t trx_cp_md_add(uint64_t a, uint64_t b) { return (a + b) & trx_cp_MD_MASK56; }
static inline uint64_t trx_cp_md_sub(uint64_t a, uint64_t b) { return (a - b) & trx_cp_MD_MASK56; }
static inline uint64_t trx_cp_md_asl(uint64_t a, unsigned n) { return (a << n) & trx_cp_MD_MASK56; }
static inline uint64_t trx_cp_md_asr(uint64_t a, unsigned n)
{
    const int64_t v = trx_cp_md_s56(a);
    const int64_t sh = v >= 0 ? v / (INT64_C(1) << n)
        : -((-v + (INT64_C(1) << n) - 1) / (INT64_C(1) << n));
    return (uint64_t)sh & trx_cp_MD_MASK56;
}
static inline uint64_t trx_cp_md_mpy(uint32_t a, uint32_t b)
{ return (uint64_t)(trx_cp_md_s24(a) * trx_cp_md_s24(b) * 2) & trx_cp_MD_MASK56; }
static inline uint64_t trx_cp_md_mac(uint64_t acc, uint32_t x, uint32_t y)
{
    return (acc + trx_cp_md_mpy(x, y)) & trx_cp_MD_MASK56;
}
static inline uint32_t trx_cp_md_acc_to_word(uint64_t a)
{
    const int64_t v = trx_cp_md_s56(a);
    if(v < -(INT64_C(0x800000) << 24)) return UINT32_C(0x800000);
    if(v >  (INT64_C(0x7fffff) << 24)) return UINT32_C(0x7fffff);
    return trx_cp_md_mid(a);
}
static inline void trx_cp_md_control_clock_seed(mdkit_control_clock *c, unsigned count,
                                         const unsigned char *knobs)
{
    c->count = count;
    for (unsigned i = 0; i < count; ++i) {
        const float v = (float)knobs[i];
        c->target[i] = c->raw[i] = c->current[i] = c->pending[i] = v;
    }
    c->block = 0; c->next_update = 0; c->publish_block = 0; c->pending_valid = 0;
}
static void trx_cp_local_clear(void *memory,unsigned size){volatile unsigned char *p=memory;while(size--)*p++=0;}

static const uint32_t trx_cp_cp_burst_decay[128]={
 0x7eb776u,0x7ebf0du,0x7ec677u,0x7ecdb5u,0x7ed4c8u,0x7edbb2u,0x7ee274u,0x7ee90du,
 0x7eef7fu,0x7ef5ccu,0x7efbf3u,0x7f01f7u,0x7f07d6u,0x7f0d93u,0x7f132eu,0x7f18a9u,
 0x7f1e02u,0x7f233du,0x7f2858u,0x7f2d55u,0x7f3235u,0x7f36f8u,0x7f3b9fu,0x7f402au,
 0x7f449bu,0x7f48f1u,0x7f4d2eu,0x7f5152u,0x7f555du,0x7f5950u,0x7f5d2cu,0x7f60f1u,
 0x7f64a0u,0x7f6839u,0x7f6bbdu,0x7f6f2cu,0x7f7286u,0x7f75cdu,0x7f7901u,0x7f7c21u,
 0x7f7f2fu,0x7f822bu,0x7f8515u,0x7f87eeu,0x7f8ab6u,0x7f8d6eu,0x7f9016u,0x7f92aeu,
 0x7f9536u,0x7f97b0u,0x7f9a1au,0x7f9c77u,0x7f9ec5u,0x7fa106u,0x7fa33au,0x7fa560u,
 0x7fa77au,0x7fa987u,0x7fab88u,0x7fad7eu,0x7faf67u,0x7fb146u,0x7fb319u,0x7fb4e1u,
 0x7fb69fu,0x7fb853u,0x7fb9fcu,0x7fbb9cu,0x7fbd32u,0x7fbebeu,0x7fc042u,0x7fc1bcu,
 0x7fc32eu,0x7fc497u,0x7fc5f7u,0x7fc750u,0x7fc8a0u,0x7fc9e9u,0x7fcb2au,0x7fcc64u,
 0x7fcd96u,0x7fcec2u,0x7fcfe6u,0x7fd104u,0x7fd21bu,0x7fd32bu,0x7fd435u,0x7fd539u,
 0x7fd637u,0x7fd72fu,0x7fd822u,0x7fd90fu,0x7fd9f6u,0x7fdad8u,0x7fdbb4u,0x7fdc8cu,
 0x7fdd5eu,0x7fde2cu,0x7fdef5u,0x7fdfb9u,0x7fe079u,0x7fe134u,0x7fe1ebu,0x7fe29eu,
 0x7fe34cu,0x7fe3f7u,0x7fe49du,0x7fe540u,0x7fe5dfu,0x7fe67au,0x7fe712u,0x7fe7a6u,
 0x7fe836u,0x7fe8c4u,0x7fe94eu,0x7fe9d4u,0x7fea58u,0x7fead9u,0x7feb56u,0x7febd1u,
 0x7fec49u,0x7fecbeu,0x7fed30u,0x7feda0u,0x7fee0du,0x7fee78u,0x7feee0u,0x7fef46u,
};



#ifndef TRX_COMPACT_MODEL_MATH
#define TRX_COMPACT_MODEL_MATH
static inline double trx_cp_trx_model_exp(double x) {
    const double ln2=0.69314718055994530942;
    int n=(int)(x/ln2+(x<0?-0.5:0.5));
    double r=x-n*ln2,term=1,sum=1;
    for(unsigned k=1;k<=18;++k) { term*=r/k;sum+=term; }
    while(n>0) {sum*=2;--n;}
    while(n<0) {sum*=0.5;++n;}
    return sum;
}
static inline double trx_cp_trx_model_cos(double x) {
    double term=1,sum=1;
    for(unsigned k=1;k<=18;++k) {term*=-x*x/((2*k-1)*(2*k));sum+=term;}
    return sum;
}
static inline long long trx_model_round(double x) {return (long long)(x+(x<0?-0.5:0.5));}
static inline uint32_t trx_cp_trx_model_word(double v) {
    if (v > 8388607.0) v = 8388607.0;
    if (v < -8388608.0) v = -8388608.0;
    return (uint32_t)(int32_t)trx_model_round(v) & 0xffffffu;
}
static inline double trx_cp_trx_model_cheb(const double *c,unsigned n,double x) {
    double b1=0,b2=0;
    for(unsigned k=n;k>1;--k) { double b=2*x*b1-b2+c[k-1]; b2=b1;b1=b; }
    return x*b1-b2+c[0];
}
#endif
static const double trx_cp_trx_model_offset_a_c[30]={-232.399680398,153.649305252,-56.7606534091,23.5448649658,-10.6578480114,4.79767127404,-81.056751598,22.7077837039,-2.96244673295,0.299532376803,0.0532892400568,-0.0105212089343,-39.1668372957,14.9894597998,-2.21767366486,0.405478971463,-0.0117738691504,0.138342962517,-15.9882689259,7.39288842873,-1.35951208431,0.190096146761,0.00637058241878,-0.116429618127,-5.5138140884,3.0706297243,-0.547032748071,0.15553066531,0.0144368154093,-0.0443455945788};
static const double trx_cp_trx_model_offset_b_c[13]={-357.643862343,-265.373321533,59.3405978631,-17.7956076723,5.97881763422,-2.11982610249,0.784178870543,-0.267558388544,0.0889657720231,0.0650633677486,0.00493977326511,-0.0123225741715,-0.021290158449};
static inline int trx_model_offset(unsigned i) {
    if(i>=128) return (int)trx_model_round(trx_cp_trx_model_cheb(trx_cp_trx_model_offset_b_c,13,2.0*(i-128)/127.0-1.0));
    unsigned part=i<8?0:i<16?1:i<32?2:i<64?3:4;
    static const unsigned trx_cp_bounds[]={0,8,16,32,64,128};
    unsigned lo=trx_cp_bounds[part],hi=trx_cp_bounds[part+1];
    return (int)trx_model_round(trx_cp_trx_model_cheb(trx_cp_trx_model_offset_a_c+part*6,6,2.0*(i-lo)/(hi-lo-1)-1.0));
}
static inline uint32_t trx_cp_trx_model_memory(unsigned i) {
    if(i<256) return (uint32_t)trx_model_offset(i)&0xffffffu;
    unsigned x=i<4352?i-256:i-4352;
    double t=trx_cp_trx_model_exp(0.003372927270490704*x+(-12.768360569597025));
    return trx_cp_trx_model_word(5931641.6632877039*(i<4352?trx_cp_trx_model_cos(t):trx_cp_trx_model_exp(-t)));
}


#ifndef TRX_COMPACT_MODEL_MATH
#define TRX_COMPACT_MODEL_MATH
static inline double trx_cp_trx_model_exp(double x) {
    const double ln2=0.69314718055994530942;
    int n=(int)(x/ln2+(x<0?-0.5:0.5));
    double r=x-n*ln2,term=1,sum=1;
    for(unsigned k=1;k<=18;++k) { term*=r/k;sum+=term; }
    while(n>0) {sum*=2;--n;}
    while(n<0) {sum*=0.5;++n;}
    return sum;
}
static inline double trx_cp_trx_model_cos(double x) {
    double term=1,sum=1;
    for(unsigned k=1;k<=18;++k) {term*=-x*x/((2*k-1)*(2*k));sum+=term;}
    return sum;
}
static inline long long trx_model_round(double x) {return (long long)(x+(x<0?-0.5:0.5));}
static inline uint32_t trx_cp_trx_model_word(double v) {
    if (v > 8388607.0) v = 8388607.0;
    if (v < -8388608.0) v = -8388608.0;
    return (uint32_t)(int32_t)trx_model_round(v) & 0xffffffu;
}
static inline double trx_cp_trx_model_cheb(const double *c,unsigned n,double x) {
    double b1=0,b2=0;
    for(unsigned k=n;k>1;--k) { double b=2*x*b1-b2+c[k-1]; b2=b1;b1=b; }
    return x*b1-b2+c[0];
}
#endif
static const double trx_cp_trx_model_room_fit[6]={11.979709774543092,-0.73214836196116828,-0.0013655690927017951,0.00016527944454415415,-1.4301789011970526e-05,1.3929757696795704e-06};
static inline uint32_t trx_cp_trx_model_room_decay(unsigned raw) {
    unsigned q=raw>16383?63:raw>>8;
    double x=2.0*q/63.0-1.0;
    return trx_cp_trx_model_word(8388608.0-trx_cp_trx_model_exp(trx_cp_trx_model_cheb(trx_cp_trx_model_room_fit,6,x)));
}

static uint32_t trx_cp_room_decay(unsigned i){return trx_cp_trx_model_room_decay(i); }



static uint32_t trx_cp_cp_filter_a(unsigned i){return (uint32_t)trx_model_offset(i)&0xffffffu;}
static uint32_t trx_cp_cp_filter_b(unsigned i){return (uint32_t)trx_model_offset(128+i)&0xffffffu;}
static uint32_t trx_cp_cp_norm(unsigned i){return trx_cp_trx_model_memory(0x900+i);}
static uint32_t trx_cp_cp_filter(unsigned i){return trx_cp_trx_model_memory(0x1900+i);}



static inline void trx_cp_cp_coefficients(uint32_t position,uint32_t width,uint32_t *feedback,uint32_t *square)
{
 unsigned norm=(position+trx_cp_cp_filter_a(width))&0xffffff;
 unsigned reson=(position+trx_cp_cp_filter_b(width))&0xffffff;
 uint32_t coefficient=trx_cp_cp_filter(reson);
 *feedback=trx_cp_md_acc_to_word(trx_cp_md_asl(trx_cp_md_mpy(coefficient,trx_cp_cp_norm(norm)),1));
 *square=trx_cp_md_acc_to_word(trx_cp_md_sub(0,trx_cp_md_mpy(coefficient,coefficient)));
}
static inline void trx_cp_cp_filter4(uint32_t *w,unsigned input_base,uint32_t history[4],uint32_t position,uint32_t width)
{
 uint32_t feedback,square;trx_cp_cp_coefficients(position,width,&feedback,&square);
 unsigned ni=(position+trx_cp_cp_filter_a(width))&0xffffff,ri=(position+trx_cp_cp_filter_b(width))&0xffffff;
 uint64_t doubled=trx_cp_md_asl(trx_cp_md_mpy(trx_cp_cp_filter(ri),trx_cp_cp_norm(ni)),1);
 uint32_t feed=trx_cp_md_acc_to_word(trx_cp_md_sub(trx_cp_md_from24(0x400000),trx_cp_md_add(doubled,trx_cp_md_from24(square))));
 w[0]=history[0];w[1]=history[1];w[input_base-2]=history[3];w[input_base-1]=history[2];
 unsigned h=0,input=input_base+1,delay=input_base-2,adjacent=input_base-1;
 uint64_t first=trx_cp_md_mac(trx_cp_md_mpy(square,w[0]),feedback,w[1]);first=trx_cp_md_mac(first,w[input_base],feed);
 for(unsigned i=0;i<32;++i){
  first=trx_cp_md_asl(first,1);
  uint64_t second=trx_cp_md_mpy(square,w[delay++]);second=trx_cp_md_mac(second,feedback,w[adjacent++]);
  uint32_t output=trx_cp_md_acc_to_word(first);second=trx_cp_md_mac(second,output,feed);w[h]=output;h^=1;
  second=trx_cp_md_asl(second,1);
  first=trx_cp_md_mpy(square,w[h]);h^=1;first=trx_cp_md_mac(first,feedback,w[h]);h^=1;
  first=trx_cp_md_mac(first,w[input--],feed);w[input]=trx_cp_md_acc_to_word(second);input+=2;
 }
 history[0]=w[h];h^=1;history[1]=w[h];history[2]=w[adjacent];history[3]=w[adjacent-1];
}
static inline void trx_cp_cp_filter6(uint32_t *w,uint32_t *aux,unsigned input_base,unsigned output_base,unsigned history_base,uint32_t history[6],uint32_t position,uint32_t width)
{
 uint32_t feedback,square;trx_cp_cp_coefficients(position,width,&feedback,&square);
 w[history_base]=history[1];w[history_base+1]=history[0];
 w[output_base]=history[3];w[output_base+1]=history[2];
 w[input_base]=history[5];w[input_base+1]=history[4];
 unsigned h=history_base,src=input_base+1,dst=output_base+1;
 uint64_t first=trx_cp_md_mpy(square,w[h]);h^=1;first=trx_cp_md_mac(first,w[h],feedback);
 uint32_t input=w[input_base];unsigned coefficient=1;
 const uint32_t feed[4]={square,0x400000,0x800000,0x400000};
 for(unsigned i=0;i<32;++i){
  first=trx_cp_md_mac(first,input,feed[coefficient]);coefficient=(coefficient+1)&3;input=w[src++];
  first=trx_cp_md_mac(first,input,feed[coefficient]);coefficient=(coefficient+1)&3;input=w[src--];
  first=trx_cp_md_mac(first,input,feed[coefficient]);coefficient=(coefficient+1)&3;
  uint32_t old_aux=aux[dst--];(void)old_aux;
  first=trx_cp_md_asl(first,1);uint32_t old_output=w[dst++];uint32_t square_word=feed[coefficient];coefficient=(coefficient+1)&3;
  uint64_t second=trx_cp_md_mpy(square_word,old_output);uint32_t recent=w[dst++];
  second=trx_cp_md_mac(second,feedback,recent);uint32_t output=trx_cp_md_acc_to_word(first);
  second=trx_cp_md_mac(second,output,feed[coefficient]);coefficient=(coefficient+1)&3;
  uint32_t old_history=w[h];h^=1;second=trx_cp_md_mac(second,old_history,feed[coefficient]);coefficient=(coefficient+1)&3;
  second=trx_cp_md_mac(second,w[h],feed[coefficient]);coefficient=(coefficient+1)&3;
  w[h]=output;h^=1;
  second=trx_cp_md_asl(second,1);uint32_t saved=w[h];h^=1;
  first=trx_cp_md_mpy(feed[coefficient],saved);coefficient=(coefficient+1)&3;
  uint32_t next_input=w[src++];first=trx_cp_md_mac(first,output,feedback);w[dst]=trx_cp_md_acc_to_word(second);input=next_input;
 }
 history[0]=w[h];h^=1;history[1]=w[h];history[2]=w[dst];history[3]=w[dst-1];history[4]=w[src];history[5]=w[src-1];
}





enum { CP_CLAP_SEED, CP_SECOND_SEED, CP_BURST_DECAY, CP_ROOM_DECAY, CP_BODY_FREQUENCY, CP_BODY_WIDTH, CP_TONE_FREQUENCY, CP_TONE_WIDTH, CP_ROOM_FREQUENCY, CP_ROOM_WIDTH, CP_ROOM_LEVEL, CP_FIRST_BURST_BLOCKS, CP_SECOND_BURST_BLOCKS, CP_THIRD_BURST_BLOCKS };
static void trx_cp_cp_controls(mdkit_trx_cp *s){
 uint32_t r[8];for(unsigned i=0;i<8;++i)r[i]=(unsigned)(s->clock.raw[i]*128.0f);
 uint32_t*p=s->parameters;
 p[CP_CLAP_SEED]=r[0]*281+3774873;p[CP_SECOND_SEED]=((r[0]*281)>>1)+3355443;
 p[CP_BURST_DECAY]=trx_cp_cp_burst_decay[r[3]>>7];p[CP_ROOM_DECAY]=trx_cp_room_decay(r[6]);
 p[CP_BODY_FREQUENCY]=(r[1]>>5)+704;p[CP_BODY_WIDTH]=(r[2]+48)>>8;p[CP_TONE_FREQUENCY]=(r[1]>>8)+1696;p[CP_TONE_WIDTH]=p[CP_BODY_WIDTH];
 p[CP_ROOM_FREQUENCY]=(r[7]>>5)+1024;p[CP_ROOM_WIDTH]=64;p[CP_ROOM_LEVEL]=(r[5]<<9)&0xfffe00;
 uint32_t quotient=(r[4]*49181)>>16;uint32_t remainder=((r[4]-quotient)&65535)>>1;
 p[CP_FIRST_BURST_BLOCKS]=(((quotient+remainder)&65535)>>9)+1;
 p[CP_SECOND_BURST_BLOCKS]=((r[4]*16811)>>23)+1;
 quotient=(r[4]*32721)>>16;remainder=((r[4]-quotient)&65535)>>1;
 p[CP_THIRD_BURST_BLOCKS]=((quotient+remainder)>>9)+1;
 for(unsigned j=0;j<3;++j)s->burst_count[j]=s->parameters[11+j];
 s->burst_seed[0]=s->parameters[CP_CLAP_SEED];s->burst_seed[1]=s->parameters[CP_SECOND_SEED];
 s->burst_decay[0]=s->parameters[CP_BURST_DECAY];s->room_amplitude=s->parameters[CP_ROOM_LEVEL];
}
static void trx_cp_cp_init(void *state){
 mdkit_trx_cp *s=state;trx_cp_local_clear(s,sizeof *s);trx_cp_md_control_clock_seed(&s->clock,8,mdkit_trx_cp_defaults);s->clock.next_update=2;
 for(unsigned i=0;i<8;++i)s->last_raw[i]=(unsigned)mdkit_trx_cp_defaults[i] * 128u;
 s->noise_a=0x29a;s->noise_b=1;trx_cp_cp_controls(s);
}
static void trx_cp_cp_set(void *state,unsigned index,unsigned value){mdkit_trx_cp*s=state;if(index<8){s->queued_targets[index]=value>127?127:value;s->target_mask|=1u<<index;}}
static void trx_cp_cp_trigger(void *state){((mdkit_trx_cp*)state)->pending_trigger=1;}
static void trx_cp_cp_boundary(mdkit_trx_cp*s){
 mdkit_control_clock*c=&s->clock;
 if(c->block==c->next_update){for(unsigned i=0;i<8;++i){uint32_t r=(uint32_t)(128.0f*c->raw[i]);s->last_raw[i]=r;c->raw[i]=(float)((3*r+128*(uint32_t)c->target[i])/4)/128.0f;}c->next_update+=12;}
 for(unsigned i=0;i<8;++i)if(s->target_mask&(1u<<i))c->target[i]=(float)s->queued_targets[i];s->target_mask=0;
 if(s->pending_trigger){trx_cp_cp_controls(s);if(!s->triggered){s->noise_a=0x29a;s->noise_b=1;for(unsigned i=0;i<4;++i)s->prefilter[i]=s->tone_filter[i]=0;for(unsigned i=0;i<6;++i)s->body_filter[i]=0;}
  s->burst_amplitude=0;s->burst_seed[2]=s->parameters[CP_SECOND_SEED];s->burst_seed[3]=0x333333;
  s->burst_decay[1]=s->burst_decay[2]=s->burst_decay[3]=s->parameters[CP_BURST_DECAY];s->pending_trigger=0;s->triggered=1;
 }++c->block;
}
static void trx_cp_cp_noise(mdkit_trx_cp*s,uint32_t*out){uint32_t first=s->noise_a,second=s->noise_b;for(unsigned i=0;i<32;++i){uint32_t sum=(first+second)&0xffffff;out[i]=sum;first=second;second=sum;}s->noise_a=first;s->noise_b=second;}
static void trx_cp_cp_block(mdkit_trx_cp*s){
 uint32_t*w=s->workspace;trx_cp_cp_noise(s,w+96);
 trx_cp_cp_filter4(w,96,s->prefilter,s->parameters[CP_ROOM_FREQUENCY],s->parameters[CP_ROOM_WIDTH]);
 for(unsigned i=0;i<32;++i)s->auxiliary[96+i]=w[96+i];
 trx_cp_cp_noise(s,w+96);
 unsigned stage=3;for(unsigned j=0;j<3;++j)if(s->burst_count[j]){stage=j;--s->burst_count[j];break;}
 s->burst_amplitude=trx_cp_md_acc_to_word(trx_cp_md_add(trx_cp_md_from24(s->burst_amplitude),trx_cp_md_from24(s->burst_seed[stage])));s->burst_seed[stage]=0;
 for(unsigned i=0;i<32;++i){uint32_t amplitude=s->burst_amplitude;w[96+i]=trx_cp_md_acc_to_word(trx_cp_md_mpy(amplitude,w[96+i]));s->burst_amplitude=trx_cp_md_acc_to_word(trx_cp_md_mpy(s->burst_decay[stage],amplitude));}
 trx_cp_cp_filter6(w,s->auxiliary,94,32,0,s->body_filter,s->parameters[CP_BODY_FREQUENCY],s->parameters[CP_BODY_WIDTH]);
 trx_cp_cp_filter4(w,32,s->tone_filter,s->parameters[CP_TONE_FREQUENCY],s->parameters[CP_TONE_WIDTH]);
 uint32_t room=s->room_amplitude;s->room_amplitude=trx_cp_md_acc_to_word(trx_cp_md_mpy(room,s->parameters[CP_ROOM_DECAY]));
 for(unsigned i=0;i<32;++i)s->block[i]=trx_cp_md_acc_to_word(trx_cp_md_asr(trx_cp_md_mac(trx_cp_md_from24(w[32+i]),room,s->auxiliary[96+i]),2));
}
static void trx_cp_cp_render(void*state,float*out,unsigned frames){mdkit_trx_cp*s=state;while(frames){if(!s->pos){trx_cp_cp_boundary(s);trx_cp_cp_block(s);}unsigned n=32-s->pos;if(n>frames)n=frames;for(unsigned i=0;i<n;++i)out[i]=s->triggered?((float)trx_cp_md_s24(s->block[s->pos+i])/8388608.0f):0.0f;out+=n;frames-=n;s->pos=(s->pos+n)&31;}}
static const char*const trx_cp_names[8]={"CLPY","TONE","HARD","RICH","RATE","ROOM","RSIZ","RTUN"};
const mdkit_voice mdkit_voice_trx_cp={"TRX-CP",19,8,trx_cp_names,sizeof(mdkit_trx_cp),trx_cp_cp_init,trx_cp_cp_set,trx_cp_cp_trigger,trx_cp_cp_render, mdkit_trx_cp_defaults
};
