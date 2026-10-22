/* SPDX-License-Identifier: CC0-1.0
 * TRX-MA standalone synthesis voice.
 */
#include "mdkit_trx_ma.h"

/* ---- Default controls --------------------------------------------------- */
/* Active controls are in descriptor order; unused clock slots stay at 64. */
static const unsigned char mdkit_trx_ma_defaults[MDKIT_MAX_CONTROLS] = {
    32, 32, 0, 0, 64, 64, 64, 64
};
#define trx_ma_MD_MASK24 UINT32_C(0xffffff)
#define trx_ma_MD_MASK48 UINT64_C(0xffffffffffff)
#define trx_ma_MD_MASK56 UINT64_C(0xffffffffffffff)
#define trx_ma_MD_VOICE_BLOCK 32u
enum { trx_ma_MD_TRX_PUBLISH=1,trx_ma_MD_TRX_TRIGGER=2,trx_ma_MD_TRX_FIRST=4 };
static inline int64_t trx_ma_md_signed_bits(uint64_t word, unsigned bits)
{
    const uint64_t sign = UINT64_C(1) << (bits - 1);
    const uint64_t mask = (UINT64_C(1) << bits) - 1;
    word &= mask;
    return (word & sign) ? (int64_t)word - (int64_t)(UINT64_C(1) << bits)
                         : (int64_t)word;
}
static inline int64_t trx_ma_md_s24(uint32_t w) { return trx_ma_md_signed_bits(w & trx_ma_MD_MASK24, 24); }
static inline int64_t trx_ma_md_s56(uint64_t a) { return trx_ma_md_signed_bits(a & trx_ma_MD_MASK56, 56); }
static inline uint32_t trx_ma_md_mid(uint64_t a) { return (uint32_t)((a >> 24) & trx_ma_MD_MASK24); }
static inline uint64_t trx_ma_md_from24(uint32_t w)
{ return ((uint64_t)(trx_ma_md_s24(w) * (INT64_C(1) << 24))) & trx_ma_MD_MASK56; }
static inline uint64_t trx_ma_md_add(uint64_t a, uint64_t b) { return (a + b) & trx_ma_MD_MASK56; }
static inline uint64_t trx_ma_md_sub(uint64_t a, uint64_t b) { return (a - b) & trx_ma_MD_MASK56; }
static inline uint64_t trx_ma_md_asl(uint64_t a, unsigned n) { return (a << n) & trx_ma_MD_MASK56; }
static inline uint64_t trx_ma_md_asr(uint64_t a, unsigned n)
{
    const int64_t v = trx_ma_md_s56(a);
    const int64_t sh = v >= 0 ? v / (INT64_C(1) << n)
        : -((-v + (INT64_C(1) << n) - 1) / (INT64_C(1) << n));
    return (uint64_t)sh & trx_ma_MD_MASK56;
}
static inline uint64_t trx_ma_md_mpy(uint32_t a, uint32_t b)
{ return (uint64_t)(trx_ma_md_s24(a) * trx_ma_md_s24(b) * 2) & trx_ma_MD_MASK56; }
static inline uint64_t trx_ma_md_mac(uint64_t acc, uint32_t x, uint32_t y)
{
    return (acc + trx_ma_md_mpy(x, y)) & trx_ma_MD_MASK56;
}
static inline uint32_t trx_ma_md_acc_to_word(uint64_t a)
{
    const int64_t v = trx_ma_md_s56(a);
    if(v < -(INT64_C(0x800000) << 24)) return UINT32_C(0x800000);
    if(v >  (INT64_C(0x7fffff) << 24)) return UINT32_C(0x7fffff);
    return trx_ma_md_mid(a);
}
static inline void trx_ma_md_control_clock_seed(mdkit_control_clock *c, unsigned count,
                                         const unsigned char *knobs)
{
    c->count = count;
    for (unsigned i = 0; i < count; ++i) {
        const float v = (float)knobs[i];
        c->target[i] = c->raw[i] = c->current[i] = c->pending[i] = v;
    }
    c->block = 0; c->next_update = 0; c->publish_block = 0; c->pending_valid = 0;
}
static void trx_ma_local_clear(void *memory,unsigned size){volatile unsigned char *p=memory;while(size--)*p++=0;}




#ifndef TRX_COMPACT_MODEL_MATH
#define TRX_COMPACT_MODEL_MATH
static inline double trx_ma_trx_model_exp(double x) {
    const double ln2=0.69314718055994530942;
    int n=(int)(x/ln2+(x<0?-0.5:0.5));
    double r=x-n*ln2,term=1,sum=1;
    for(unsigned k=1;k<=18;++k) { term*=r/k;sum+=term; }
    while(n>0) {sum*=2;--n;}
    while(n<0) {sum*=0.5;++n;}
    return sum;
}
static inline double trx_ma_trx_model_cos(double x) {
    double term=1,sum=1;
    for(unsigned k=1;k<=18;++k) {term*=-x*x/((2*k-1)*(2*k));sum+=term;}
    return sum;
}
static inline long long trx_model_round(double x) {return (long long)(x+(x<0?-0.5:0.5));}
static inline uint32_t trx_ma_trx_model_word(double v) {
    if (v > 8388607.0) v = 8388607.0;
    if (v < -8388608.0) v = -8388608.0;
    return (uint32_t)(int32_t)trx_model_round(v) & 0xffffffu;
}
static inline double trx_ma_trx_model_cheb(const double *c,unsigned n,double x) {
    double b1=0,b2=0;
    for(unsigned k=n;k>1;--k) { double b=2*x*b1-b2+c[k-1]; b2=b1;b1=b; }
    return x*b1-b2+c[0];
}
#endif
static const double trx_ma_trx_model_offset_a_c[30]={-232.399680398,153.649305252,-56.7606534091,23.5448649658,-10.6578480114,4.79767127404,-81.056751598,22.7077837039,-2.96244673295,0.299532376803,0.0532892400568,-0.0105212089343,-39.1668372957,14.9894597998,-2.21767366486,0.405478971463,-0.0117738691504,0.138342962517,-15.9882689259,7.39288842873,-1.35951208431,0.190096146761,0.00637058241878,-0.116429618127,-5.5138140884,3.0706297243,-0.547032748071,0.15553066531,0.0144368154093,-0.0443455945788};
static const double trx_ma_trx_model_offset_b_c[13]={-357.643862343,-265.373321533,59.3405978631,-17.7956076723,5.97881763422,-2.11982610249,0.784178870543,-0.267558388544,0.0889657720231,0.0650633677486,0.00493977326511,-0.0123225741715,-0.021290158449};
static inline int trx_model_offset(unsigned i) {
    if(i>=128) return (int)trx_model_round(trx_ma_trx_model_cheb(trx_ma_trx_model_offset_b_c,13,2.0*(i-128)/127.0-1.0));
    unsigned part=i<8?0:i<16?1:i<32?2:i<64?3:4;
    static const unsigned trx_ma_bounds[]={0,8,16,32,64,128};
    unsigned lo=trx_ma_bounds[part],hi=trx_ma_bounds[part+1];
    return (int)trx_model_round(trx_ma_trx_model_cheb(trx_ma_trx_model_offset_a_c+part*6,6,2.0*(i-lo)/(hi-lo-1)-1.0));
}
static inline uint32_t trx_ma_trx_model_memory(unsigned i) {
    if(i<256) return (uint32_t)trx_model_offset(i)&0xffffffu;
    unsigned x=i<4352?i-256:i-4352;
    double t=trx_ma_trx_model_exp(0.003372927270490704*x+(-12.768360569597025));
    return trx_ma_trx_model_word(5931641.6632877039*(i<4352?trx_ma_trx_model_cos(t):trx_ma_trx_model_exp(-t)));
}



static uint32_t trx_ma_cp_filter_a(unsigned i){return (uint32_t)trx_model_offset(i)&0xffffffu;}
static uint32_t trx_ma_cp_filter_b(unsigned i){return (uint32_t)trx_model_offset(128+i)&0xffffffu;}
static uint32_t trx_ma_cp_norm(unsigned i){return trx_ma_trx_model_memory(0x900+i);}
static uint32_t trx_ma_cp_filter(unsigned i){return trx_ma_trx_model_memory(0x1900+i);}



static inline void trx_ma_cp_coefficients(uint32_t position,uint32_t width,uint32_t *feedback,uint32_t *square)
{
 unsigned norm=(position+trx_ma_cp_filter_a(width))&0xffffff;
 unsigned reson=(position+trx_ma_cp_filter_b(width))&0xffffff;
 uint32_t coefficient=trx_ma_cp_filter(reson);
 *feedback=trx_ma_md_acc_to_word(trx_ma_md_asl(trx_ma_md_mpy(coefficient,trx_ma_cp_norm(norm)),1));
 *square=trx_ma_md_acc_to_word(trx_ma_md_sub(0,trx_ma_md_mpy(coefficient,coefficient)));
}

static inline void trx_ma_cp_filter6(uint32_t *w,uint32_t *aux,unsigned input_base,unsigned output_base,unsigned history_base,uint32_t history[6],uint32_t position,uint32_t width)
{
 uint32_t feedback,square;trx_ma_cp_coefficients(position,width,&feedback,&square);
 w[history_base]=history[1];w[history_base+1]=history[0];
 w[output_base]=history[3];w[output_base+1]=history[2];
 w[input_base]=history[5];w[input_base+1]=history[4];
 unsigned h=history_base,src=input_base+1,dst=output_base+1;
 uint64_t first=trx_ma_md_mpy(square,w[h]);h^=1;first=trx_ma_md_mac(first,w[h],feedback);
 uint32_t input=w[input_base];unsigned coefficient=1;
 const uint32_t feed[4]={square,0x400000,0x800000,0x400000};
 for(unsigned i=0;i<32;++i){
  first=trx_ma_md_mac(first,input,feed[coefficient]);coefficient=(coefficient+1)&3;input=w[src++];
  first=trx_ma_md_mac(first,input,feed[coefficient]);coefficient=(coefficient+1)&3;input=w[src--];
  first=trx_ma_md_mac(first,input,feed[coefficient]);coefficient=(coefficient+1)&3;
  uint32_t old_aux=aux[dst--];(void)old_aux;
  first=trx_ma_md_asl(first,1);uint32_t old_output=w[dst++];uint32_t square_word=feed[coefficient];coefficient=(coefficient+1)&3;
  uint64_t second=trx_ma_md_mpy(square_word,old_output);uint32_t recent=w[dst++];
  second=trx_ma_md_mac(second,feedback,recent);uint32_t output=trx_ma_md_acc_to_word(first);
  second=trx_ma_md_mac(second,output,feed[coefficient]);coefficient=(coefficient+1)&3;
  uint32_t old_history=w[h];h^=1;second=trx_ma_md_mac(second,old_history,feed[coefficient]);coefficient=(coefficient+1)&3;
  second=trx_ma_md_mac(second,w[h],feed[coefficient]);coefficient=(coefficient+1)&3;
  w[h]=output;h^=1;
  second=trx_ma_md_asl(second,1);uint32_t saved=w[h];h^=1;
  first=trx_ma_md_mpy(feed[coefficient],saved);coefficient=(coefficient+1)&3;
  uint32_t next_input=w[src++];first=trx_ma_md_mac(first,output,feedback);w[dst]=trx_ma_md_acc_to_word(second);input=next_input;
 }
 history[0]=w[h];h^=1;history[1]=w[h];history[2]=w[dst];history[3]=w[dst-1];history[4]=w[src];history[5]=w[src-1];
}



enum { MA_ATTACK_BLOCKS, MA_ATTACK_STEP, MA_SUSTAIN_BLOCKS, MA_RELEASE_BLOCKS, MA_RELEASE_STEP, MA_RATTLE_THRESHOLD, MA_RATTLE_FREQUENCY, MA_RATTLE_WIDTH, MA_TONE_FREQUENCY, MA_SOURCE_FREQUENCY, MA_ROOM_STEP };
static void trx_ma_ma_controls(mdkit_trx_ma*s){
 uint32_t r[8];for(unsigned i=0;i<8;++i)r[i]=(uint32_t)(s->clock.raw[i]*128.0f);
 uint32_t*p=s->parameters;
 p[MA_ATTACK_BLOCKS]=(r[0]>>6)+10;p[MA_ATTACK_STEP]=0x800000/((r[0]>>1)+42);p[MA_SUSTAIN_BLOCKS]=(r[1]>>6)+1;
 p[MA_RELEASE_BLOCKS]=(r[2]>>6)+10;p[MA_RELEASE_STEP]=0x800000/((r[2]>>3)+16);p[MA_RATTLE_THRESHOLD]=7879228-(r[3]<<7);
 p[MA_RATTLE_FREQUENCY]=(r[6]>>6)+1536;p[MA_RATTLE_WIDTH]=r[7]>>8;p[MA_TONE_FREQUENCY]=(r[6]>>6)+1504;p[MA_SOURCE_FREQUENCY]=(r[5]>>4)+1024;
 p[MA_ROOM_STEP]=(r[4]<<6)/(p[MA_ATTACK_BLOCKS]+p[MA_SUSTAIN_BLOCKS]+p[MA_RELEASE_BLOCKS]);
 s->attack_blocks=p[MA_ATTACK_BLOCKS];s->sustain_blocks=p[MA_SUSTAIN_BLOCKS];s->release_blocks=p[MA_RELEASE_BLOCKS];
}
static void trx_ma_ma_init(void*state){mdkit_trx_ma*s=state;trx_ma_local_clear(s,sizeof *s);trx_ma_md_control_clock_seed(&s->clock,8,mdkit_trx_ma_defaults);s->clock.next_update=2;for(unsigned i=0;i<8;++i)s->last_raw[i]=(unsigned)mdkit_trx_ma_defaults[i] * 128u;s->noise_a=0x29a;s->noise_b=1;trx_ma_ma_controls(s);}
static void trx_ma_ma_set(void *state,unsigned index,unsigned value){mdkit_trx_ma*s=state;if(index<8){s->queued_targets[index]=value>127?127:value;s->target_mask|=1u<<index;}}
static void trx_ma_ma_trigger(void *state){((mdkit_trx_ma*)state)->pending_trigger=1;}
static void trx_ma_ma_noise(mdkit_trx_ma*s,uint32_t*out){uint32_t first=s->noise_a,second=s->noise_b;for(unsigned i=0;i<32;++i){uint32_t sum=(first+second)&0xffffff;out[i]=sum;first=second;second=sum;}s->noise_a=first;s->noise_b=second;}

static void trx_ma_ma_boundary(mdkit_trx_ma*s){
 mdkit_control_clock*c=&s->clock;
 if(c->block==c->next_update){for(unsigned i=0;i<8;++i){uint32_t r=(uint32_t)(128.0f*c->raw[i]);s->last_raw[i]=r;c->raw[i]=(float)((3*r+128*(uint32_t)c->target[i])/4)/128.0f;}c->next_update+=12;}
 for(unsigned i=0;i<8;++i)if(s->target_mask&(1u<<i))c->target[i]=(float)s->queued_targets[i];s->target_mask=0;
 if(s->pending_trigger){trx_ma_ma_controls(s);if(!s->triggered){s->noise_a=0x29a;s->noise_b=1;for(unsigned i=0;i<6;++i)s->source_filter[i]=s->rattle_filter[i]=s->tone_filter[i]=0;}
  s->envelope=s->rattle_growth=s->room_amplitude=0;s->rattle_attack=0x7a3e22;s->rattle_decay=0x450d02;
  s->pending_trigger=0;s->triggered=1;
 }++c->block;
}
static void trx_ma_ma_block(mdkit_trx_ma*s){
 uint32_t*w=s->workspace,*p=s->parameters;
 trx_ma_ma_noise(s,w+2);
 trx_ma_cp_filter6(w,s->auxiliary,0,70,112,s->source_filter,p[MA_SOURCE_FREQUENCY],127);
 for(unsigned i=0;i<32;++i)s->auxiliary[32+i]=w[70+i];
 uint32_t growth=s->rattle_growth,rate=s->rattle_rate;uint64_t floor=trx_ma_md_from24(s->rattle_floor);
 for(unsigned i=0;i<32;++i){
  uint64_t noise=trx_ma_md_from24(w[33-i]);
  if(trx_ma_md_s56(noise)>=trx_ma_md_s56(trx_ma_md_from24(p[MA_RATTLE_THRESHOLD]))){growth=0xfffcb9;floor=trx_ma_md_from24(w[33-i]);if(trx_ma_md_s56(floor)<0)floor=trx_ma_md_sub(0,floor);floor=trx_ma_md_sub(0,floor);rate=s->rattle_decay;}
  uint64_t value=trx_ma_md_mpy(rate,growth);
  if(floor){value=trx_ma_md_add(value,trx_ma_md_from24(growth));growth=trx_ma_md_acc_to_word(value);if(trx_ma_md_s56(value)<=trx_ma_md_s56(floor)){floor=trx_ma_md_sub(0,floor);rate=s->rattle_attack;growth=trx_ma_md_acc_to_word(floor);floor=0;}}
  else growth=trx_ma_md_acc_to_word(value);
  w[34+i]=trx_ma_md_acc_to_word(value);
 }
 s->rattle_growth=growth;s->rattle_floor=trx_ma_md_acc_to_word(floor);s->rattle_rate=rate;
 trx_ma_cp_filter6(w,s->auxiliary,34,70,112,s->rattle_filter,p[MA_RATTLE_FREQUENCY],p[MA_RATTLE_WIDTH]);
 for(unsigned i=0;i<32;++i)w[70+i]=trx_ma_md_acc_to_word(trx_ma_md_asr(trx_ma_md_from24(w[70+i]),3));
 trx_ma_cp_filter6(w,s->auxiliary,68,2,112,s->tone_filter,p[MA_TONE_FREQUENCY],0);
 s->room_amplitude=trx_ma_md_acc_to_word(trx_ma_md_add(trx_ma_md_from24(s->room_amplitude),trx_ma_md_from24(p[MA_ROOM_STEP])));
 for(unsigned i=0;i<32;++i)w[2+i]=trx_ma_md_acc_to_word(trx_ma_md_mac(trx_ma_md_from24(w[2+i]),s->auxiliary[32+i],s->room_amplitude));
 unsigned stage=3;if(s->attack_blocks){stage=0;--s->attack_blocks;}else if(s->sustain_blocks){stage=1;--s->sustain_blocks;}else if(s->release_blocks){stage=2;--s->release_blocks;}
 uint64_t envelope=trx_ma_md_from24(s->envelope);
 for(unsigned i=0;i<32;++i){
  if(stage==0)envelope=trx_ma_md_add(envelope,trx_ma_md_from24(p[MA_ATTACK_STEP]));if(stage==2)envelope=trx_ma_md_sub(envelope,trx_ma_md_from24(p[MA_RELEASE_STEP]));
  uint64_t output=stage==3?0:stage==1?trx_ma_md_from24(w[2+i]):trx_ma_md_mpy(w[2+i],trx_ma_md_acc_to_word(envelope));
  s->block[i]=trx_ma_md_acc_to_word(trx_ma_md_asr(output,2));
 }if(stage==0||stage==2)s->envelope=trx_ma_md_acc_to_word(envelope);
}
static void trx_ma_ma_render(void*state,float*out,unsigned frames){mdkit_trx_ma*s=state;while(frames){if(!s->pos){trx_ma_ma_boundary(s);trx_ma_ma_block(s);}unsigned n=32-s->pos;if(n>frames)n=frames;for(unsigned i=0;i<n;++i)out[i]=s->triggered?((float)trx_ma_md_s24(s->block[s->pos+i])/8388608.0f):0.0f;out+=n;frames-=n;s->pos=(s->pos+n)&31;}}
static const char*const trx_ma_names[8]={"ATT","SUS","REV","DAMP","RATL","RTYP","TONE","HARD"};
const mdkit_voice mdkit_voice_trx_ma={"TRX-MA",25,8,trx_ma_names,sizeof(mdkit_trx_ma),trx_ma_ma_init,trx_ma_ma_set,trx_ma_ma_trigger,trx_ma_ma_render, mdkit_trx_ma_defaults
};
