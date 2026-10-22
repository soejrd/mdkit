/* SPDX-License-Identifier: CC0-1.0
 * TRX-SD standalone synthesis voice.
 */
#include "mdkit_trx_sd.h"

/* ---- Default controls --------------------------------------------------- */
/* Active controls are in descriptor order; unused clock slots stay at 64. */
static const unsigned char mdkit_trx_sd_defaults[MDKIT_MAX_CONTROLS] = {
    35, 7, 0, 64, 127, 0, 100, 90
};
#define trx_sd_MD_MASK24 UINT32_C(0xffffff)
#define trx_sd_MD_MASK48 UINT64_C(0xffffffffffff)
#define trx_sd_MD_MASK56 UINT64_C(0xffffffffffffff)
#define trx_sd_MD_VOICE_BLOCK 32u
enum { trx_sd_MD_TRX_PUBLISH=1,trx_sd_MD_TRX_TRIGGER=2,trx_sd_MD_TRX_FIRST=4 };
static inline int64_t trx_sd_md_signed_bits(uint64_t word, unsigned bits)
{
    const uint64_t sign = UINT64_C(1) << (bits - 1);
    const uint64_t mask = (UINT64_C(1) << bits) - 1;
    word &= mask;
    return (word & sign) ? (int64_t)word - (int64_t)(UINT64_C(1) << bits)
                         : (int64_t)word;
}
static inline int64_t trx_sd_md_s24(uint32_t w) { return trx_sd_md_signed_bits(w & trx_sd_MD_MASK24, 24); }
static inline int64_t trx_sd_md_s56(uint64_t a) { return trx_sd_md_signed_bits(a & trx_sd_MD_MASK56, 56); }
static inline uint32_t trx_sd_md_mid(uint64_t a) { return (uint32_t)((a >> 24) & trx_sd_MD_MASK24); }
static inline uint32_t trx_sd_md_low(uint64_t a) { return (uint32_t)(a & trx_sd_MD_MASK24); }
static inline uint64_t trx_sd_md_from24(uint32_t w)
{ return ((uint64_t)(trx_sd_md_s24(w) * (INT64_C(1) << 24))) & trx_sd_MD_MASK56; }
static inline uint64_t trx_sd_md_from48(uint64_t w)
{ return (uint64_t)trx_sd_md_signed_bits(w & trx_sd_MD_MASK48, 48) & trx_sd_MD_MASK56; }
static inline uint64_t trx_sd_md_setmid(uint64_t a, uint32_t w)
{ return (a & ~(UINT64_C(0xffffff) << 24)) | ((uint64_t)(w & trx_sd_MD_MASK24) << 24); }
static inline uint64_t trx_sd_md_add(uint64_t a, uint64_t b) { return (a + b) & trx_sd_MD_MASK56; }
static inline uint64_t trx_sd_md_sub(uint64_t a, uint64_t b) { return (a - b) & trx_sd_MD_MASK56; }
static inline uint64_t trx_sd_md_asl(uint64_t a, unsigned n) { return (a << n) & trx_sd_MD_MASK56; }
static inline uint64_t trx_sd_md_asr(uint64_t a, unsigned n)
{
    const int64_t v = trx_sd_md_s56(a);
    const int64_t sh = v >= 0 ? v / (INT64_C(1) << n)
        : -((-v + (INT64_C(1) << n) - 1) / (INT64_C(1) << n));
    return (uint64_t)sh & trx_sd_MD_MASK56;
}
static inline uint64_t trx_sd_md_mpy(uint32_t a, uint32_t b)
{ return (uint64_t)(trx_sd_md_s24(a) * trx_sd_md_s24(b) * 2) & trx_sd_MD_MASK56; }
static inline uint64_t trx_sd_md_mac(uint64_t acc, uint32_t x, uint32_t y)
{
    return (acc + trx_sd_md_mpy(x, y)) & trx_sd_MD_MASK56;
}
static inline uint64_t trx_sd_md_and_mid(uint64_t a, uint32_t mask)
{ return trx_sd_md_setmid(a, trx_sd_md_mid(a) & mask); }
static inline uint64_t trx_sd_md_pack48(uint32_t hi, uint32_t lo)
{ return ((uint64_t)(hi & trx_sd_MD_MASK24) << 24) | (lo & trx_sd_MD_MASK24); }
static inline uint32_t trx_sd_md_acc_to_word(uint64_t a)
{
    const int64_t v = trx_sd_md_s56(a);
    if(v < -(INT64_C(0x800000) << 24)) return UINT32_C(0x800000);
    if(v >  (INT64_C(0x7fffff) << 24)) return UINT32_C(0x7fffff);
    return trx_sd_md_mid(a);
}
static inline void trx_sd_md_control_clock_seed(mdkit_control_clock *c, unsigned count,
                                         const unsigned char *knobs)
{
    c->count = count;
    for (unsigned i = 0; i < count; ++i) {
        const float v = (float)knobs[i];
        c->target[i] = c->raw[i] = c->current[i] = c->pending[i] = v;
    }
    c->block = 0; c->next_update = 0; c->publish_block = 0; c->pending_valid = 0;
}
static uint32_t trx_sd_local_sine(unsigned index) {
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

static const uint32_t trx_sd_sd_DEC_cc128[128]={
 0x7fc8a0u,0x7fc9e9u,0x7fcb2au,0x7fcc64u,0x7fcd96u,0x7fcec2u,0x7fcfe6u,0x7fd104u,
 0x7fd21bu,0x7fd32bu,0x7fd435u,0x7fd539u,0x7fd637u,0x7fd72fu,0x7fd822u,0x7fd90fu,
 0x7fd9f6u,0x7fdad8u,0x7fdbb4u,0x7fdc8cu,0x7fdd5eu,0x7fde2cu,0x7fdef5u,0x7fdfb9u,
 0x7fe079u,0x7fe134u,0x7fe1ebu,0x7fe29eu,0x7fe34cu,0x7fe3f7u,0x7fe49du,0x7fe540u,
 0x7fe5dfu,0x7fe67au,0x7fe712u,0x7fe7a6u,0x7fe836u,0x7fe8c4u,0x7fe94eu,0x7fe9d4u,
 0x7fea58u,0x7fead9u,0x7feb56u,0x7febd1u,0x7fec49u,0x7fecbeu,0x7fed30u,0x7feda0u,
 0x7fee0du,0x7fee78u,0x7feee0u,0x7fef46u,0x7fefa9u,0x7ff00au,0x7ff069u,0x7ff0c6u,
 0x7ff120u,0x7ff178u,0x7ff1cfu,0x7ff223u,0x7ff275u,0x7ff2c6u,0x7ff314u,0x7ff361u,
 0x7ff3acu,0x7ff3f5u,0x7ff43du,0x7ff483u,0x7ff4c7u,0x7ff50au,0x7ff54bu,0x7ff58au,
 0x7ff5c9u,0x7ff605u,0x7ff641u,0x7ff67au,0x7ff6b3u,0x7ff6eau,0x7ff720u,0x7ff755u,
 0x7ff789u,0x7ff7bbu,0x7ff7ecu,0x7ff81cu,0x7ff84bu,0x7ff879u,0x7ff8a5u,0x7ff8d1u,
 0x7ff8fcu,0x7ff925u,0x7ff94eu,0x7ff976u,0x7ff99du,0x7ff9c3u,0x7ff9e8u,0x7ffa0cu,
 0x7ffa2fu,0x7ffa52u,0x7ffa74u,0x7ffa95u,0x7ffab5u,0x7ffad4u,0x7ffaf3u,0x7ffb11u,
 0x7ffb2eu,0x7ffb4bu,0x7ffb67u,0x7ffb82u,0x7ffb9du,0x7ffbb7u,0x7ffbd0u,0x7ffbe9u,
 0x7ffc02u,0x7ffc19u,0x7ffc30u,0x7ffc47u,0x7ffc5du,0x7ffc73u,0x7ffc88u,0x7ffc9du,
 0x7ffcb1u,0x7ffcc4u,0x7ffcd8u,0x7ffceau,0x7ffcfdu,0x7ffd0fu,0x7ffd20u,0x7ffd31u,
};
static const uint32_t trx_sd_sd_NOISE_DEC_cc128[128]={
 0x7fd637u,0x7fd637u,0x7fd72fu,0x7fd72fu,0x7fd822u,0x7fd822u,0x7fd90fu,0x7fd90fu,
 0x7fd9f6u,0x7fd9f6u,0x7fdad8u,0x7fdad8u,0x7fdbb4u,0x7fdbb4u,0x7fdc8cu,0x7fdc8cu,
 0x7fdd5eu,0x7fdd5eu,0x7fde2cu,0x7fde2cu,0x7fdef5u,0x7fdef5u,0x7fdfb9u,0x7fdfb9u,
 0x7fe079u,0x7fe079u,0x7fe134u,0x7fe134u,0x7fe1ebu,0x7fe1ebu,0x7fe29eu,0x7fe29eu,
 0x7fe34cu,0x7fe34cu,0x7fe3f7u,0x7fe3f7u,0x7fe49du,0x7fe49du,0x7fe540u,0x7fe540u,
 0x7fe5dfu,0x7fe5dfu,0x7fe67au,0x7fe67au,0x7fe712u,0x7fe712u,0x7fe7a6u,0x7fe7a6u,
 0x7fe836u,0x7fe836u,0x7fe8c4u,0x7fe8c4u,0x7fe94eu,0x7fe94eu,0x7fe9d4u,0x7fe9d4u,
 0x7fea58u,0x7fea58u,0x7fead9u,0x7fead9u,0x7feb56u,0x7feb56u,0x7febd1u,0x7febd1u,
 0x7fec49u,0x7fec49u,0x7fecbeu,0x7fecbeu,0x7fed30u,0x7fed30u,0x7feda0u,0x7feda0u,
 0x7fee0du,0x7fee0du,0x7fee78u,0x7fee78u,0x7feee0u,0x7feee0u,0x7fef46u,0x7fef46u,
 0x7fefa9u,0x7fefa9u,0x7ff00au,0x7ff00au,0x7ff069u,0x7ff069u,0x7ff0c6u,0x7ff0c6u,
 0x7ff120u,0x7ff120u,0x7ff178u,0x7ff178u,0x7ff1cfu,0x7ff1cfu,0x7ff223u,0x7ff223u,
 0x7ff275u,0x7ff275u,0x7ff2c6u,0x7ff2c6u,0x7ff314u,0x7ff314u,0x7ff361u,0x7ff361u,
 0x7ff3acu,0x7ff3acu,0x7ff3f5u,0x7ff3f5u,0x7ff43du,0x7ff43du,0x7ff483u,0x7ff483u,
 0x7ff4c7u,0x7ff4c7u,0x7ff50au,0x7ff50au,0x7ff54bu,0x7ff54bu,0x7ff58au,0x7ff58au,
 0x7ff5c9u,0x7ff5c9u,0x7ff605u,0x7ff605u,0x7ff641u,0x7ff641u,0x7ff67au,0x7ff67au,
};
static const uint32_t trx_sd_sd_benv_raw[80]={
 0x67eb0cu,0x686c0du,0x68eaabu,0x6966ecu,0x69e0d9u,0x6a587au,0x6acdd6u,0x6b40f7u,
 0x6bb1e3u,0x6c20a2u,0x6c8d3eu,0x6cf7beu,0x6d602au,0x6dc68au,0x6e2ae6u,0x6e8d46u,
 0x6eedb3u,0x6f4c34u,0x6fa8d1u,0x700392u,0x705c80u,0x70b3a1u,0x7108fdu,0x715c9du,
 0x71ae88u,0x71fec5u,0x724d5cu,0x729a55u,0x72e5b6u,0x732f87u,0x7377cfu,0x73be96u,
 0x7403e2u,0x7447bau,0x748a25u,0x74cb2bu,0x750ad1u,0x75491eu,0x75861au,0x75c1c9u,
 0x75fc33u,0x76355eu,0x766d50u,0x76a410u,0x76d9a2u,0x770e0eu,0x774158u,0x777387u,
 0x77a4a0u,0x77d4a9u,0x7803a7u,0x78319fu,0x785e98u,0x788a96u,0x78b59du,0x78dfb4u,
 0x7908dfu,0x793123u,0x795884u,0x797f08u,0x79a4b3u,0x79c98au,0x79ed90u,0x7a10cau,
 0x7a333du,0x7a54edu,0x7a75ddu,0x7a9613u,0x7ab591u,0x7ad45cu,0x7af277u,0x7b0fe7u,
 0x7b2caeu,0x7b48d1u,0x7b6453u,0x7b7f38u,0x7b9982u,0x7bb336u,0x7bcc56u,0x7be4e5u,
};
static const uint32_t trx_sd_sd_clip_raw[198]={
 0x00002fu,0x000044u,0x000062u,0x00008bu,0x0000c3u,0x00010fu,0x000175u,0x0001fcu,
 0x0002aeu,0x000394u,0x0004beu,0x00063au,0x00081cu,0x000a7au,0x000d6du,0x001113u,
 0x00158eu,0x001b05u,0x0021a1u,0x002994u,0x003311u,0x003e54u,0x004b9bu,0x005b2bu,
 0x006d4du,0x00824fu,0x009a84u,0x00b642u,0x00d5e4u,0x00f9c7u,0x01224bu,0x014fd3u,
 0x0182c1u,0x01bb7au,0x01fa5fu,0x023fd5u,0x028c3bu,0x02dfeeu,0x033b4au,0x039ea3u,
 0x040a4cu,0x047e90u,0x04fbb2u,0x0581f2u,0x061187u,0x06aa9eu,0x074d5fu,0x07f9e8u,
 0x08b04eu,0x09709cu,0x0a3ad8u,0x0b0efau,0x0becf4u,0x0cd4aeu,0x0dc60au,0x0ec0deu,
 0x0fc4fau,0x10d227u,0x11e825u,0x1306aeu,0x142d76u,0x155c2cu,0x169278u,0x17cffeu,
 0x19145eu,0x1a5f35u,0x1bb01bu,0x1d06a7u,0x1e626eu,0x1fc303u,0x2127f8u,0x2290deu,
 0x23fd48u,0x256cc6u,0x26deebu,0x28534bu,0x29c97cu,0x2b4113u,0x2cb9abu,0x2e32dfu,
 0x2fac4cu,0x312595u,0x329e5du,0x34164au,0x358d09u,0x370246u,0x3875b3u,0x39e706u,
 0x3b55f7u,0x3cc243u,0x3e2ba9u,0x3f91efu,0x40f4dau,0x425438u,0x43afd6u,0x450787u,
 0x465b20u,0x47aa7cu,0x48f576u,0x4a3beeu,0x4b7dc8u,0x4cbae9u,0x4df33bu,0x4f26a9u,
 0x505522u,0x517e97u,0x52a2fbu,0x53c245u,0x54dc6cu,0x55f16cu,0x570140u,0x580be8u,
 0x591163u,0x5a11b4u,0x5b0cddu,0x5c02e5u,0x5cf3d2u,0x5ddfabu,0x5ec67au,0x5fa849u,
 0x608524u,0x615d16u,0x62302du,0x62fe77u,0x63c802u,0x648cdfu,0x654d1cu,0x6608cbu,
 0x66bffdu,0x6772c3u,0x68212fu,0x68cb54u,0x697143u,0x6a130fu,0x6ab0cbu,0x6b4a8au,
 0x6be05fu,0x6c725eu,0x6d0099u,0x6d8b23u,0x6e1211u,0x6e9574u,0x6f1560u,0x6f91e7u,
 0x700b1eu,0x708115u,0x70f3e0u,0x716390u,0x71d039u,0x7239ecu,0x72a0bau,0x7304b5u,
 0x7365eeu,0x73c477u,0x74205fu,0x7479b8u,0x74d091u,0x7524fbu,0x757705u,0x75c6beu,
 0x761436u,0x765f7bu,0x76a89bu,0x76efa6u,0x7734a9u,0x7777b1u,0x77b8cdu,0x77f808u,
 0x783570u,0x787112u,0x78aaf9u,0x78e332u,0x7919c9u,0x794ec8u,0x79823au,0x79b42cu,
 0x79e4a7u,0x7a13b6u,0x7a4163u,0x7a6db8u,0x7a98bfu,0x7ac280u,0x7aeb05u,0x7b1258u,
 0x7b3880u,0x7b5d86u,0x7b8173u,0x7ba44eu,0x7bc61fu,0x7be6efu,0x7c06c3u,0x7c25a5u,
 0x7c439au,0x7c60a9u,0x7c7cdau,0x7c9832u,0x7cb2b9u,0x7ccc73u,
};



#ifndef TRX_COMPACT_MODEL_MATH
#define TRX_COMPACT_MODEL_MATH
static inline double trx_sd_trx_model_exp(double x) {
    const double ln2=0.69314718055994530942;
    int n=(int)(x/ln2+(x<0?-0.5:0.5));
    double r=x-n*ln2,term=1,sum=1;
    for(unsigned k=1;k<=18;++k) { term*=r/k;sum+=term; }
    while(n>0) {sum*=2;--n;}
    while(n<0) {sum*=0.5;++n;}
    return sum;
}
static inline double trx_sd_trx_model_cos(double x) {
    double term=1,sum=1;
    for(unsigned k=1;k<=18;++k) {term*=-x*x/((2*k-1)*(2*k));sum+=term;}
    return sum;
}
static inline long long trx_model_round(double x) {return (long long)(x+(x<0?-0.5:0.5));}
static inline uint32_t trx_sd_trx_model_word(double v) {
    if (v > 8388607.0) v = 8388607.0;
    if (v < -8388608.0) v = -8388608.0;
    return (uint32_t)(int32_t)trx_model_round(v) & 0xffffffu;
}
static inline double trx_sd_trx_model_cheb(const double *c,unsigned n,double x) {
    double b1=0,b2=0;
    for(unsigned k=n;k>1;--k) { double b=2*x*b1-b2+c[k-1]; b2=b1;b1=b; }
    return x*b1-b2+c[0];
}
#endif
static const double trx_sd_trx_model_offset_a_c[30]={-232.399680398,153.649305252,-56.7606534091,23.5448649658,-10.6578480114,4.79767127404,-81.056751598,22.7077837039,-2.96244673295,0.299532376803,0.0532892400568,-0.0105212089343,-39.1668372957,14.9894597998,-2.21767366486,0.405478971463,-0.0117738691504,0.138342962517,-15.9882689259,7.39288842873,-1.35951208431,0.190096146761,0.00637058241878,-0.116429618127,-5.5138140884,3.0706297243,-0.547032748071,0.15553066531,0.0144368154093,-0.0443455945788};
static const double trx_sd_trx_model_offset_b_c[13]={-357.643862343,-265.373321533,59.3405978631,-17.7956076723,5.97881763422,-2.11982610249,0.784178870543,-0.267558388544,0.0889657720231,0.0650633677486,0.00493977326511,-0.0123225741715,-0.021290158449};
static inline int trx_model_offset(unsigned i) {
    if(i>=128) return (int)trx_model_round(trx_sd_trx_model_cheb(trx_sd_trx_model_offset_b_c,13,2.0*(i-128)/127.0-1.0));
    unsigned part=i<8?0:i<16?1:i<32?2:i<64?3:4;
    static const unsigned trx_sd_bounds[]={0,8,16,32,64,128};
    unsigned lo=trx_sd_bounds[part],hi=trx_sd_bounds[part+1];
    return (int)trx_model_round(trx_sd_trx_model_cheb(trx_sd_trx_model_offset_a_c+part*6,6,2.0*(i-lo)/(hi-lo-1)-1.0));
}
static inline uint32_t trx_sd_trx_model_memory(unsigned i) {
    if(i<256) return (uint32_t)trx_model_offset(i)&0xffffffu;
    unsigned x=i<4352?i-256:i-4352;
    double t=trx_sd_trx_model_exp(0.003372927270490704*x+(-12.768360569597025));
    return trx_sd_trx_model_word(5931641.6632877039*(i<4352?trx_sd_trx_model_cos(t):trx_sd_trx_model_exp(-t)));
}



static uint32_t trx_sd_sd_norm(unsigned i){return trx_sd_trx_model_memory(0x900+i);}
static uint32_t trx_sd_sd_filter(unsigned i){return trx_sd_trx_model_memory(0x1900+i);}

static double trx_sd_attack_exp(double x) {
    const double ln2=0.693147180559945309417232121458;
    int n=(int)(x/ln2+(x<0?-0.5:0.5));double r=x-n*ln2,term=1.0,sum=1.0;
    for(unsigned k=1;k<=18;++k){term*=r/(double)k;sum+=term;}
    while(n>0){sum*=2.0;--n;}while(n<0){sum*=0.5;++n;}return sum;
}
/* Broadband transient: a deterministic integer noise source through a compact
 * Hamming-windowed sinc band-pass, plus sparse filtered grains for the snap.
 * The envelope supplies the short attack and exponential decay. */
#define TRX_SD_ATTACK_KERNEL 33u
static const double trx_sd_attack_kernel[TRX_SD_ATTACK_KERNEL]={
  -0.0001843832816393393,0.0014723283878510715,-0.0026493910779054209,-0.011745544768867078,
  -0.013873354726815813,-0.003184813103064421,-0.0033587037844761045,-0.038201593087613644,
  -0.073690215806129833,-0.04900569373979248,0.0072507599852698829,-0.025521868698211495,
  -0.16767820788667159,-0.21958816027823083,0.022963517906413884,0.44392003913335437,
  0.65683567882013794,0.44392003913335437,0.022963517906413884,-0.21958816027823083,
  -0.16767820788667159,-0.025521868698211495,0.0072507599852698829,-0.04900569373979248,
  -0.073690215806129833,-0.038201593087613644,-0.0033587037844761045,-0.003184813103064421,
  -0.013873354726815813,-0.011745544768867078,-0.0026493910779054209,0.0014723283878510715,
  -0.0001843832816393393
};
static const uint16_t trx_sd_attack_grain_pos[53]={
  54,66,78,97,114,129,153,169,182,200,234,257,278,292,304,318,333,349,363,381,394,415,431,465,478,495,
  515,531,550,563,575,593,608,654,667,679,697,716,761,775,787,817,839,873,888,917,966,990,1005,1117,
  1181,1315,1376
};
static const double trx_sd_attack_grain_amp[53]={
  -0.61,-0.45,-0.37,0.63,-0.96,0.54,0.6,-1.0,-0.57,-0.47,-0.62,0.76,0.54,-0.76,0.54,-0.55,-0.67,-0.72,
  -0.53,-0.88,0.49,-0.42,-0.44,0.51,-0.51,0.42,0.71,-0.87,-0.58,0.51,-0.66,-0.41,-0.44,-0.64,-0.41,
  -0.61,-0.44,-0.51,-0.41,-0.38,-0.54,0.39,-0.53,-0.56,-0.51,-0.57,-0.51,-0.37,-0.51,-0.36,-0.48,-0.37,
  -0.35
};
static uint32_t trx_sd_attack_hash(unsigned i)
{
    uint32_t z=(i+1u)*UINT32_C(0x9e3779b1);
    z^=z>>16; z*=UINT32_C(0x85ebca6b); z^=z>>13; z*=UINT32_C(0xc2b2ae35); z^=z>>16;
    return z;
}
static double trx_sd_attack_envelope(unsigned i)
{
    const double i_d=(double)i;
    const double rise=1.0-((i_d/8.0<50.0)?trx_sd_attack_exp(-i_d/8.0):0.0);
    const double hold=i_d>512.0?i_d-512.0:0.0;
    return rise*trx_sd_attack_exp(-hold/850.0);
}
static uint32_t sd_attack(unsigned index)
{
    static const unsigned prefix=TRX_SD_ATTACK_KERNEL/2u;
    double sum=0.0;
    for(unsigned k=0;k<TRX_SD_ATTACK_KERNEL;++k) {
        int pos=(int)index+prefix-(int)k;
        if(pos<0)continue;
        const double u=((double)(trx_sd_attack_hash((unsigned)pos)>>8)/8388608.0)-1.0;
        sum+=u*trx_sd_attack_kernel[k];
    }
    for(unsigned g=0;g<53u;++g) {
        int local=(int)index-(int)trx_sd_attack_grain_pos[g];
        if(local<0||local>=(int)TRX_SD_ATTACK_KERNEL)continue;
        sum+=trx_sd_attack_grain_amp[g]*trx_sd_attack_kernel[local];
    }
    sum*=trx_sd_attack_envelope(index);
    const double scaled=sum*0.5156756282484805*8388608.0;
    if(scaled>8388607.0)return UINT32_C(0x7fffff);
    if(scaled<-8388608.0)return UINT32_C(0x800000);
    return (uint32_t)(int32_t)(scaled+(scaled<0?-0.5:0.5))&UINT32_C(0xffffff);
}









static void trx_sd_sd_controls(mdkit_trx_sd *s)
{
    uint32_t raw[8];
    for (unsigned i=0; i<8; ++i) raw[i]=(uint32_t)(s->clock.current[i]*128.0f);
    s->decay=trx_sd_sd_DEC_cc128[raw[1]>>7];
    s->noise_decay=trx_sd_sd_NOISE_DEC_cc128[raw[1]>>7];
    s->tone_a=0x7fffff-(raw[5]<<8);
    s->tone_b=raw[5]<<8;
    s->snap=raw[4]<<9;
    s->snap_boost=raw[4]<8192 ? 0 : (((raw[4]-8192)*(raw[4]-8192)>>4)&0xfffc00);
    s->pitch_a=(raw[0]>>2)+0x1000;
    s->pitch_b=((s->pitch_a>>6)*(0xb9+(raw[6]>>9)))>>1;
    s->shape_decay=trx_sd_sd_clip_raw[(raw[7]*0x62b3)>>21];
    s->bump_step=(raw[2]*raw[2])>>18;
    s->filter_index=(raw[0]>>5)+0x400;
    s->bump_decay=trx_sd_sd_benv_raw[(raw[3]*5)>>10];
}

static void trx_sd_sd_init(void *state)
{
    mdkit_trx_sd *s=state;

    trx_sd_md_control_clock_seed(&s->clock,8,mdkit_trx_sd_defaults);
    s->clock.next_update=2;
    s->pos=s->pending_trigger=s->triggered=s->target_mask=0;
    for (unsigned i=0;i<8;++i) { s->queued_targets[i]=mdkit_trx_sd_defaults[i]; s->last_raw[i]=(unsigned)mdkit_trx_sd_defaults[i] * 128u; }
    s->body_amplitude=s->noise_amplitude=s->shape=s->bump=s->attack_cursor=0;
    s->phase_a=s->phase_b=0;
    s->noise_a=0x29a; s->noise_b=1;
    s->filter_a=s->filter_b=s->filter_c=s->filter_d=s->last_output=0;
    for (unsigned i=0;i<96;++i) s->workspace[i]=0;
    for (unsigned i=0;i<32;++i) s->block[i]=0;
    trx_sd_sd_controls(s);
}

static void trx_sd_sd_set_control(void *state,unsigned index,unsigned value)
{
    mdkit_trx_sd *s=state;
    if(index>=8)return;
    s->queued_targets[index]=value>127?127:value;
    s->target_mask|=1u<<index;
}
static void trx_sd_sd_trigger(void *state) { ((mdkit_trx_sd *)state)->pending_trigger=1; }


static void trx_sd_sd_boundary(mdkit_trx_sd *s)
{
    mdkit_control_clock *c=&s->clock;
    if(c->pending_valid && c->block>=c->publish_block) {
        for(unsigned i=0;i<8;++i)c->current[i]=c->pending[i];
        c->pending_valid=0;
    }
    if(c->block==c->next_update) {
        int changed=0;
        for(unsigned i=0;i<8;++i) {
            c->pending[i]=c->raw[i];
            uint32_t raw=(uint32_t)(128.0f*c->raw[i]);
            changed|=raw!=s->last_raw[i]; s->last_raw[i]=raw;
            c->raw[i]=(float)((3*raw+128*(uint32_t)c->target[i])/4)/128.0f;
        }
        c->pending_valid=changed; c->publish_block=c->block+1; c->next_update+=12;
    }
    for(unsigned i=0;i<8;++i)
        if(s->target_mask&(1u<<i))c->target[i]=(float)s->queued_targets[i];
    s->target_mask=0;
    if(s->pending_trigger)for(unsigned i=0;i<8;++i)c->current[i]=c->raw[i];
    trx_sd_sd_controls(s);
    if(s->pending_trigger) {
        if(!s->triggered) {
            s->noise_a=0x29a; s->noise_b=1;
            s->filter_a=s->filter_b=s->filter_c=s->filter_d=0;
        }
        s->body_amplitude=trx_sd_md_acc_to_word(trx_sd_md_add(trx_sd_md_from24(0x50c335),trx_sd_md_from24(s->body_amplitude)));
        s->shape=0x7eb852;
        uint64_t previous=trx_sd_md_from24(s->last_output);
        if(trx_sd_md_s56(previous)<0)previous=trx_sd_md_sub(0,previous);
        s->noise_amplitude=trx_sd_md_acc_to_word(trx_sd_md_add(trx_sd_md_from24(0x4ccccd),trx_sd_md_asl(previous,4)));
        s->bump=s->attack_cursor=0; s->phase_a=s->phase_b=0;
        s->pending_trigger=0; s->triggered=1;
    }
    ++c->block;
}

static void trx_sd_sd_tone(uint64_t *phase,uint32_t pitch,uint32_t bump,uint32_t gain,uint32_t *out)
{
    uint64_t step=trx_sd_md_asr(trx_sd_md_add(trx_sd_md_asr(trx_sd_md_from24(pitch),1),trx_sd_md_from24(bump)),4);
    uint64_t running=trx_sd_md_from48(*phase);
    for(unsigned i=0;i<32;++i) {
        unsigned index=trx_sd_md_mid(running)&0x7fff;
        running=trx_sd_md_and_mid(trx_sd_md_add(running,step),0x7fff);
        out[i]=trx_sd_md_acc_to_word(trx_sd_md_mpy(trx_sd_local_sine(index),gain));
    }
    *phase=trx_sd_md_pack48(trx_sd_md_mid(running),trx_sd_md_low(running));
}


static void trx_sd_sd_noise_filter(mdkit_trx_sd *s)
{
    uint32_t *w=s->workspace;
    uint32_t resonance=trx_sd_sd_filter(s->filter_index-0x241);
    uint64_t product=trx_sd_md_mpy(resonance,trx_sd_sd_norm(s->filter_index-3));
    uint64_t negative_square=trx_sd_md_sub(0,trx_sd_md_mpy(resonance,resonance));
    uint64_t doubled=trx_sd_md_asl(product,1);
    uint32_t feedback=trx_sd_md_acc_to_word(doubled);
    uint64_t feed=trx_sd_md_sub(trx_sd_md_from24(0x400000),trx_sd_md_add(doubled,trx_sd_md_from24(trx_sd_md_acc_to_word(negative_square))));
    uint32_t square=trx_sd_md_acc_to_word(negative_square), input_gain=trx_sd_md_acc_to_word(feed);
    w[3]=s->filter_a; w[2]=s->filter_b;
    w[30]=s->filter_c; w[31]=s->filter_d;

    unsigned history=3, input=33, delayed=30, adjacent=31;
    uint64_t first=trx_sd_md_mac(trx_sd_md_mpy(square,w[3]),feedback,w[2]);
    first=trx_sd_md_mac(first,w[32],input_gain);
    for(unsigned i=0;i<32;++i) {
        first=trx_sd_md_asl(first,1);
        uint32_t delayed_word=w[delayed++];
        uint64_t second=trx_sd_md_mpy(square,delayed_word);
        uint32_t adjacent_word=w[adjacent++];
        second=trx_sd_md_mac(second,feedback,adjacent_word);
        uint32_t first_word=trx_sd_md_acc_to_word(first);
        second=trx_sd_md_mac(second,first_word,input_gain);
        w[history]=first_word; history=history==2?3:2;
        second=trx_sd_md_asl(second,1);
        uint32_t old_history=w[history]; history=history==2?3:2;
        first=trx_sd_md_mpy(square,old_history);
        uint32_t new_history=w[history]; history=history==2?3:2;
        first=trx_sd_md_mac(first,feedback,new_history);
        uint32_t input_word=w[input--];
        first=trx_sd_md_mac(first,input_word,input_gain);
        w[input]=trx_sd_md_acc_to_word(second); input+=2;
    }
    s->filter_a=w[history]; history=history==2?3:2;
    s->filter_b=w[history];
    s->filter_d=w[adjacent]; s->filter_c=w[adjacent-1];
}

static void trx_sd_sd_block(mdkit_trx_sd *s)
{
    uint32_t *w=s->workspace;
    trx_sd_sd_tone(&s->phase_a,s->pitch_a,s->bump,s->tone_a,w+64);

    uint32_t old_bump=s->bump;
    if(s->attack_cursor>0x140)s->bump=trx_sd_md_acc_to_word(trx_sd_md_mpy(s->bump_decay,old_bump));
    else s->bump=trx_sd_md_acc_to_word(trx_sd_md_add(trx_sd_md_asl(trx_sd_md_from24(s->bump_step),1),trx_sd_md_from24(old_bump)));
    trx_sd_sd_tone(&s->phase_b,s->pitch_b,old_bump,s->tone_b,w+32);
    uint32_t old_shape=s->shape;
    s->shape=trx_sd_md_acc_to_word(trx_sd_md_mpy(s->shape_decay,old_shape));
    for(unsigned i=0;i<32;++i) {
        uint64_t sum=trx_sd_md_add(trx_sd_md_from24(w[64+i]),trx_sd_md_from24(w[32+i]));
        uint64_t extra=trx_sd_md_mac(trx_sd_md_mpy(w[64+i],old_shape),old_shape,w[32+i]);
        w[64+i]=trx_sd_md_acc_to_word(trx_sd_md_add(sum,trx_sd_md_asl(extra,6)));
    }
    for(unsigned i=0;i<32;++i) {
        uint64_t tone=trx_sd_md_mpy(s->body_amplitude,w[64+i]);
        s->body_amplitude=trx_sd_md_acc_to_word(trx_sd_md_mpy(s->body_amplitude,s->decay));
        w[64+i]=trx_sd_md_acc_to_word(tone);
    }
    uint32_t first=s->noise_a,second=s->noise_b;
    for(unsigned i=0;i<32;++i) {
        uint32_t sum=(first+second)&trx_sd_MD_MASK24;
        w[32+i]=sum;
        if(i<31){first=second;second=sum;}
        else {s->noise_a=second;s->noise_b=sum;}
    }
    trx_sd_sd_noise_filter(s);
    for(unsigned i=0;i<32;++i) {
        uint64_t noise=trx_sd_md_mpy(s->noise_amplitude,w[32+i]);
        s->noise_amplitude=trx_sd_md_acc_to_word(trx_sd_md_mpy(s->noise_amplitude,s->noise_decay));
        w[32+i]=trx_sd_md_acc_to_word(noise);
    }
    for(unsigned i=0;i<32;++i)
        w[64+i]=trx_sd_md_acc_to_word(trx_sd_md_mac(trx_sd_md_from24(w[64+i]),w[32+i],s->snap_boost));
    uint32_t cursor=s->attack_cursor;
    s->attack_cursor=trx_sd_md_acc_to_word(trx_sd_md_add(trx_sd_md_from24(cursor),trx_sd_md_from24(32)));
    for(unsigned i=0;i<32;++i) {
        uint64_t mixed=trx_sd_md_from24(w[64+i]);
        if(cursor<=0xfe0)mixed=trx_sd_md_mac(mixed,sd_attack(cursor+i),s->snap);
        s->block[i]=trx_sd_md_acc_to_word(mixed);
    }
    s->last_output=s->block[31];
}

static void trx_sd_sd_render(void *state,float *out,unsigned frames)
{
    mdkit_trx_sd *s=state;
    while(frames) {
        if(!s->pos){trx_sd_sd_boundary(s);trx_sd_sd_block(s);}
        unsigned count=32-s->pos; if(count>frames)count=frames;
        for(unsigned i=0;i<count;++i)out[i]=s->triggered?((float)trx_sd_md_s24(s->block[s->pos+i])/8388608.0f):0.0f;
        frames-=count; out+=count; s->pos=(s->pos+count)&31;
    }
}
static const char *const trx_sd_sd_names[8]={"PTCH","DEC","BUMP","BENV","SNAP","TONE","TUNE","CLIP"};
const mdkit_voice mdkit_voice_trx_sd={"TRX-SD",17,8,trx_sd_sd_names,sizeof(mdkit_trx_sd),trx_sd_sd_init,trx_sd_sd_set_control,trx_sd_sd_trigger,trx_sd_sd_render, mdkit_trx_sd_defaults
};
