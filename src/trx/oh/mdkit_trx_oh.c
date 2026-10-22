/* SPDX-License-Identifier: CC0-1.0
 * TRX-OH standalone synthesis voice.
 */
#include "mdkit_trx_oh.h"

/* ---- Default controls --------------------------------------------------- */
/* Active controls are in descriptor order; unused clock slots stay at 64. */
static const unsigned char mdkit_trx_oh_defaults[MDKIT_MAX_CONTROLS] = {
    50, 60, 8, 127, 0, 64, 64, 64
};
#define trx_oh_MD_MASK24 UINT32_C(0xffffff)
#define trx_oh_MD_MASK48 UINT64_C(0xffffffffffff)
#define trx_oh_MD_MASK56 UINT64_C(0xffffffffffffff)
#define trx_oh_MD_VOICE_BLOCK 32u
enum { trx_oh_MD_TRX_PUBLISH=1,trx_oh_MD_TRX_TRIGGER=2,trx_oh_MD_TRX_FIRST=4 };
static inline int64_t trx_oh_md_signed_bits(uint64_t word, unsigned bits)
{
    const uint64_t sign = UINT64_C(1) << (bits - 1);
    const uint64_t mask = (UINT64_C(1) << bits) - 1;
    word &= mask;
    return (word & sign) ? (int64_t)word - (int64_t)(UINT64_C(1) << bits)
                         : (int64_t)word;
}
static inline int64_t trx_oh_md_s24(uint32_t w) { return trx_oh_md_signed_bits(w & trx_oh_MD_MASK24, 24); }
static inline int64_t trx_oh_md_s56(uint64_t a) { return trx_oh_md_signed_bits(a & trx_oh_MD_MASK56, 56); }
static inline uint32_t trx_oh_md_mid(uint64_t a) { return (uint32_t)((a >> 24) & trx_oh_MD_MASK24); }
static inline uint64_t trx_oh_md_from24(uint32_t w)
{ return ((uint64_t)(trx_oh_md_s24(w) * (INT64_C(1) << 24))) & trx_oh_MD_MASK56; }
static inline uint64_t trx_oh_md_add(uint64_t a, uint64_t b) { return (a + b) & trx_oh_MD_MASK56; }
static inline uint64_t trx_oh_md_sub(uint64_t a, uint64_t b) { return (a - b) & trx_oh_MD_MASK56; }
static inline uint64_t trx_oh_md_asl(uint64_t a, unsigned n) { return (a << n) & trx_oh_MD_MASK56; }
static inline uint64_t trx_oh_md_asr(uint64_t a, unsigned n)
{
    const int64_t v = trx_oh_md_s56(a);
    const int64_t sh = v >= 0 ? v / (INT64_C(1) << n)
        : -((-v + (INT64_C(1) << n) - 1) / (INT64_C(1) << n));
    return (uint64_t)sh & trx_oh_MD_MASK56;
}
static inline uint64_t trx_oh_md_mpy(uint32_t a, uint32_t b)
{ return (uint64_t)(trx_oh_md_s24(a) * trx_oh_md_s24(b) * 2) & trx_oh_MD_MASK56; }
static inline uint64_t trx_oh_md_mac(uint64_t acc, uint32_t x, uint32_t y)
{
    return (acc + trx_oh_md_mpy(x, y)) & trx_oh_MD_MASK56;
}
static inline uint32_t trx_oh_md_acc_to_word(uint64_t a)
{
    const int64_t v = trx_oh_md_s56(a);
    if(v < -(INT64_C(0x800000) << 24)) return UINT32_C(0x800000);
    if(v >  (INT64_C(0x7fffff) << 24)) return UINT32_C(0x7fffff);
    return trx_oh_md_mid(a);
}
static inline void trx_oh_md_control_clock_seed(mdkit_control_clock *c, unsigned count,
                                         const unsigned char *knobs)
{
    c->count = count;
    for (unsigned i = 0; i < count; ++i) {
        const float v = (float)knobs[i];
        c->target[i] = c->raw[i] = c->current[i] = c->pending[i] = v;
    }
    c->block = 0; c->next_update = 0; c->publish_block = 0; c->pending_valid = 0;
}
static const uint32_t trx_oh_control_word_1[128]={
 0x7fd104u,0x7fd21bu,0x7fd32bu,0x7fd435u,0x7fd539u,0x7fd637u,0x7fd72fu,0x7fd822u,
 0x7fd90fu,0x7fd9f6u,0x7fdad8u,0x7fdbb4u,0x7fdc8cu,0x7fdd5eu,0x7fde2cu,0x7fdef5u,
 0x7fdfb9u,0x7fe079u,0x7fe134u,0x7fe1ebu,0x7fe29eu,0x7fe34cu,0x7fe3f7u,0x7fe49du,
 0x7fe540u,0x7fe5dfu,0x7fe67au,0x7fe712u,0x7fe7a6u,0x7fe836u,0x7fe8c4u,0x7fe94eu,
 0x7fe9d4u,0x7fea58u,0x7fead9u,0x7feb56u,0x7febd1u,0x7fec49u,0x7fecbeu,0x7fed30u,
 0x7feda0u,0x7fee0du,0x7fee78u,0x7feee0u,0x7fef46u,0x7fefa9u,0x7ff00au,0x7ff069u,
 0x7ff0c6u,0x7ff120u,0x7ff178u,0x7ff1cfu,0x7ff223u,0x7ff275u,0x7ff2c6u,0x7ff314u,
 0x7ff361u,0x7ff3acu,0x7ff3f5u,0x7ff43du,0x7ff483u,0x7ff4c7u,0x7ff50au,0x7ff54bu,
 0x7ff58au,0x7ff5c9u,0x7ff605u,0x7ff641u,0x7ff67au,0x7ff6b3u,0x7ff6eau,0x7ff720u,
 0x7ff755u,0x7ff789u,0x7ff7bbu,0x7ff7ecu,0x7ff81cu,0x7ff84bu,0x7ff879u,0x7ff8a5u,
 0x7ff8d1u,0x7ff8fcu,0x7ff925u,0x7ff94eu,0x7ff976u,0x7ff99du,0x7ff9c3u,0x7ff9e8u,
 0x7ffa0cu,0x7ffa2fu,0x7ffa52u,0x7ffa74u,0x7ffa95u,0x7ffab5u,0x7ffad4u,0x7ffaf3u,
 0x7ffb11u,0x7ffb2eu,0x7ffb4bu,0x7ffb67u,0x7ffb82u,0x7ffb9du,0x7ffbb7u,0x7ffbd0u,
 0x7ffbe9u,0x7ffc02u,0x7ffc19u,0x7ffc30u,0x7ffc47u,0x7ffc5du,0x7ffc73u,0x7ffc88u,
 0x7ffc9du,0x7ffcb1u,0x7ffcc4u,0x7ffcd8u,0x7ffceau,0x7ffcfdu,0x7ffd0fu,0x7ffd20u,
 0x7ffd31u,0x7ffd42u,0x7ffd52u,0x7ffd62u,0x7ffd72u,0x7ffd81u,0x7ffd90u,0x7ffd9eu,
};
static const uint32_t trx_oh_control_word_2[128]={
 0x000680u,0x000681u,0x000682u,0x000684u,0x000685u,0x000686u,0x000688u,0x000689u,
 0x00068au,0x00068cu,0x00068du,0x00068eu,0x000690u,0x000691u,0x000692u,0x000694u,
 0x000695u,0x000696u,0x000698u,0x000699u,0x00069au,0x00069cu,0x00069du,0x00069eu,
 0x0006a0u,0x0006a1u,0x0006a3u,0x0006a4u,0x0006a5u,0x0006a7u,0x0006a8u,0x0006a9u,
 0x0006abu,0x0006acu,0x0006adu,0x0006afu,0x0006b0u,0x0006b1u,0x0006b3u,0x0006b4u,
 0x0006b5u,0x0006b7u,0x0006b8u,0x0006b9u,0x0006bbu,0x0006bcu,0x0006bdu,0x0006bfu,
 0x0006c0u,0x0006c2u,0x0006c3u,0x0006c4u,0x0006c6u,0x0006c7u,0x0006c8u,0x0006cau,
 0x0006cbu,0x0006ccu,0x0006ceu,0x0006cfu,0x0006d0u,0x0006d2u,0x0006d3u,0x0006d4u,
 0x0006d6u,0x0006d7u,0x0006d8u,0x0006dau,0x0006dbu,0x0006dcu,0x0006deu,0x0006dfu,
 0x0006e1u,0x0006e2u,0x0006e3u,0x0006e5u,0x0006e6u,0x0006e7u,0x0006e9u,0x0006eau,
 0x0006ebu,0x0006edu,0x0006eeu,0x0006efu,0x0006f1u,0x0006f2u,0x0006f3u,0x0006f5u,
 0x0006f6u,0x0006f7u,0x0006f9u,0x0006fau,0x0006fbu,0x0006fdu,0x0006feu,0x000700u,
 0x000701u,0x000702u,0x000704u,0x000705u,0x000706u,0x000708u,0x000709u,0x00070au,
 0x00070cu,0x00070du,0x00070eu,0x000710u,0x000711u,0x000712u,0x000714u,0x000715u,
 0x000716u,0x000718u,0x000719u,0x00071au,0x00071cu,0x00071du,0x00071eu,0x000720u,
 0x000721u,0x000723u,0x000724u,0x000725u,0x000727u,0x000728u,0x000729u,0x00072bu,
};
static const uint32_t trx_oh_control_word_4[128]={
 0x080000u,0x07f380u,0x07e700u,0x07da80u,0x07ce00u,0x07c180u,0x07b500u,0x07a880u,
 0x079c00u,0x078f80u,0x078300u,0x077680u,0x076a00u,0x075d80u,0x075100u,0x074480u,
 0x073800u,0x072b80u,0x071f00u,0x071280u,0x070600u,0x06f980u,0x06ed00u,0x06e080u,
 0x06d400u,0x06c780u,0x06bb00u,0x06ae80u,0x06a200u,0x069580u,0x068900u,0x067c80u,
 0x067000u,0x066380u,0x065700u,0x064a80u,0x063e00u,0x063180u,0x062500u,0x061880u,
 0x060c00u,0x05ff80u,0x05f300u,0x05e680u,0x05da00u,0x05cd80u,0x05c100u,0x05b480u,
 0x05a800u,0x059b80u,0x058f00u,0x058280u,0x057600u,0x056980u,0x055d00u,0x055080u,
 0x054400u,0x053780u,0x052b00u,0x051e80u,0x051200u,0x050580u,0x04f900u,0x04ec80u,
 0x04e000u,0x04d380u,0x04c700u,0x04ba80u,0x04ae00u,0x04a180u,0x049500u,0x048880u,
 0x047c00u,0x046f80u,0x046300u,0x045680u,0x044a00u,0x043d80u,0x043100u,0x042480u,
 0x041800u,0x040b80u,0x03ff00u,0x03f280u,0x03e600u,0x03d980u,0x03cd00u,0x03c080u,
 0x03b400u,0x03a780u,0x039b00u,0x038e80u,0x038200u,0x037580u,0x036900u,0x035c80u,
 0x035000u,0x034380u,0x033700u,0x032a80u,0x031e00u,0x031180u,0x030500u,0x02f880u,
 0x02ec00u,0x02df80u,0x02d300u,0x02c680u,0x02ba00u,0x02ad80u,0x02a100u,0x029480u,
 0x028800u,0x027b80u,0x026f00u,0x026280u,0x025600u,0x024980u,0x023d00u,0x023080u,
 0x022400u,0x021780u,0x020b00u,0x01fe80u,0x01f200u,0x01e580u,0x01d900u,0x01cc80u,
};
static const uint32_t trx_oh_control_word_5[128]={
 0x000600u,0x000601u,0x000602u,0x000604u,0x000605u,0x000607u,0x000608u,0x000609u,
 0x00060bu,0x00060cu,0x00060eu,0x00060fu,0x000611u,0x000612u,0x000613u,0x000615u,
 0x000616u,0x000618u,0x000619u,0x00061bu,0x00061cu,0x00061du,0x00061fu,0x000620u,
 0x000622u,0x000623u,0x000624u,0x000626u,0x000627u,0x000629u,0x00062au,0x00062cu,
 0x00062du,0x00062eu,0x000630u,0x000631u,0x000633u,0x000634u,0x000636u,0x000637u,
 0x000638u,0x00063au,0x00063bu,0x00063du,0x00063eu,0x000640u,0x000641u,0x000642u,
 0x000644u,0x000645u,0x000647u,0x000648u,0x000649u,0x00064bu,0x00064cu,0x00064eu,
 0x00064fu,0x000651u,0x000652u,0x000653u,0x000655u,0x000656u,0x000658u,0x000659u,
 0x00065bu,0x00065cu,0x00065du,0x00065fu,0x000660u,0x000662u,0x000663u,0x000664u,
 0x000666u,0x000667u,0x000669u,0x00066au,0x00066cu,0x00066du,0x00066eu,0x000670u,
 0x000671u,0x000673u,0x000674u,0x000676u,0x000677u,0x000678u,0x00067au,0x00067bu,
 0x00067du,0x00067eu,0x000680u,0x000681u,0x000682u,0x000684u,0x000685u,0x000687u,
 0x000688u,0x000689u,0x00068bu,0x00068cu,0x00068eu,0x00068fu,0x000691u,0x000692u,
 0x000693u,0x000695u,0x000696u,0x000698u,0x000699u,0x00069bu,0x00069cu,0x00069du,
 0x00069fu,0x0006a0u,0x0006a2u,0x0006a3u,0x0006a4u,0x0006a6u,0x0006a7u,0x0006a9u,
 0x0006aau,0x0006acu,0x0006adu,0x0006aeu,0x0006b0u,0x0006b1u,0x0006b3u,0x0006b4u,
};
static const uint32_t trx_oh_control_word_7[128]={
 0x010000u,0x010700u,0x010e00u,0x011500u,0x011c00u,0x012300u,0x012a00u,0x013100u,
 0x013800u,0x013f00u,0x014600u,0x014d00u,0x015400u,0x015b00u,0x016200u,0x016900u,
 0x017000u,0x017700u,0x017e00u,0x018500u,0x018c00u,0x019300u,0x019a00u,0x01a100u,
 0x01a800u,0x01af00u,0x01b600u,0x01bd00u,0x01c400u,0x01cb00u,0x01d200u,0x01d900u,
 0x01e000u,0x01e700u,0x01ee00u,0x01f500u,0x01fc00u,0x020300u,0x020a00u,0x021100u,
 0x021800u,0x021f00u,0x022600u,0x022d00u,0x023400u,0x023b00u,0x024200u,0x024900u,
 0x025000u,0x025700u,0x025e00u,0x026500u,0x026c00u,0x027300u,0x027a00u,0x028100u,
 0x028800u,0x028f00u,0x029600u,0x029d00u,0x02a400u,0x02ab00u,0x02b200u,0x02b900u,
 0x02c000u,0x02c700u,0x02ce00u,0x02d500u,0x02dc00u,0x02e300u,0x02ea00u,0x02f100u,
 0x02f800u,0x02ff00u,0x030600u,0x030d00u,0x031400u,0x031b00u,0x032200u,0x032900u,
 0x033000u,0x033700u,0x033e00u,0x034500u,0x034c00u,0x035300u,0x035a00u,0x036100u,
 0x036800u,0x036f00u,0x037600u,0x037d00u,0x038400u,0x038b00u,0x039200u,0x039900u,
 0x03a000u,0x03a700u,0x03ae00u,0x03b500u,0x03bc00u,0x03c300u,0x03ca00u,0x03d100u,
 0x03d800u,0x03df00u,0x03e600u,0x03ed00u,0x03f400u,0x03fb00u,0x040200u,0x040900u,
 0x041000u,0x041700u,0x041e00u,0x042500u,0x042c00u,0x043300u,0x043a00u,0x044100u,
 0x044800u,0x044f00u,0x045600u,0x045d00u,0x046400u,0x046b00u,0x047200u,0x047900u,
};
static const uint32_t trx_oh_control_word_9[128]={
 0x000025u,0x00002du,0x000036u,0x00003eu,0x000047u,0x00004fu,0x000058u,0x000060u,
 0x000069u,0x000071u,0x00007au,0x000082u,0x00008bu,0x000093u,0x00009cu,0x0000a5u,
 0x0000adu,0x0000b6u,0x0000beu,0x0000c7u,0x0000cfu,0x0000d8u,0x0000e0u,0x0000e9u,
 0x0000f1u,0x0000fau,0x000102u,0x00010bu,0x000113u,0x00011cu,0x000125u,0x00012du,
 0x000136u,0x00013eu,0x000147u,0x00014fu,0x000158u,0x000160u,0x000169u,0x000171u,
 0x00017au,0x000182u,0x00018bu,0x000193u,0x00019cu,0x0001a5u,0x0001adu,0x0001b6u,
 0x0001beu,0x0001c7u,0x0001cfu,0x0001d8u,0x0001e0u,0x0001e9u,0x0001f1u,0x0001fau,
 0x000202u,0x00020bu,0x000213u,0x00021cu,0x000225u,0x00022du,0x000236u,0x00023eu,
 0x000247u,0x00024fu,0x000258u,0x000260u,0x000269u,0x000271u,0x00027au,0x000282u,
 0x00028bu,0x000293u,0x00029cu,0x0002a5u,0x0002adu,0x0002b6u,0x0002beu,0x0002c7u,
 0x0002cfu,0x0002d8u,0x0002e0u,0x0002e9u,0x0002f1u,0x0002fau,0x000302u,0x00030bu,
 0x000313u,0x00031cu,0x000325u,0x00032du,0x000336u,0x00033eu,0x000347u,0x00034fu,
 0x000358u,0x000360u,0x000369u,0x000371u,0x00037au,0x000382u,0x00038bu,0x000393u,
 0x00039cu,0x0003a5u,0x0003adu,0x0003b6u,0x0003beu,0x0003c7u,0x0003cfu,0x0003d8u,
 0x0003e0u,0x0003e9u,0x0003f1u,0x0003fau,0x000402u,0x00040bu,0x000413u,0x00041cu,
 0x000425u,0x00042du,0x000436u,0x00043eu,0x000447u,0x00044fu,0x000458u,0x000460u,
};
static void trx_oh_trx_oh_from_knobs(const float k[8],uint32_t p[10]){p[0]=0x000126u;p[1]=trx_oh_control_word_1[(unsigned)k[0]];p[2]=trx_oh_control_word_2[(unsigned)k[3]];p[3]=0x000046u;p[4]=trx_oh_control_word_4[(unsigned)k[4]];p[5]=trx_oh_control_word_5[(unsigned)k[2]];p[6]=0x000046u;p[7]=trx_oh_control_word_7[(unsigned)k[4]];p[8]=0x000001u;p[9]=trx_oh_control_word_9[(unsigned)k[1]];}
static void trx_oh_oh_convert(const uint32_t raw[8],uint32_t *p){float k[8];for(unsigned i=0;i<8;++i)k[i]=(float)(raw[i]>>7);trx_oh_trx_oh_from_knobs(k,p);}



#ifndef TRX_COMPACT_MODEL_MATH
#define TRX_COMPACT_MODEL_MATH
static inline double trx_oh_trx_model_exp(double x) {
    const double ln2=0.69314718055994530942;
    int n=(int)(x/ln2+(x<0?-0.5:0.5));
    double r=x-n*ln2,term=1,sum=1;
    for(unsigned k=1;k<=18;++k) { term*=r/k;sum+=term; }
    while(n>0) {sum*=2;--n;}
    while(n<0) {sum*=0.5;++n;}
    return sum;
}
static inline double trx_oh_trx_model_cos(double x) {
    double term=1,sum=1;
    for(unsigned k=1;k<=18;++k) {term*=-x*x/((2*k-1)*(2*k));sum+=term;}
    return sum;
}
static inline long long trx_model_round(double x) {return (long long)(x+(x<0?-0.5:0.5));}
static inline uint32_t trx_oh_trx_model_word(double v) {
    if (v > 8388607.0) v = 8388607.0;
    if (v < -8388608.0) v = -8388608.0;
    return (uint32_t)(int32_t)trx_model_round(v) & 0xffffffu;
}
static inline double trx_oh_trx_model_cheb(const double *c,unsigned n,double x) {
    double b1=0,b2=0;
    for(unsigned k=n;k>1;--k) { double b=2*x*b1-b2+c[k-1]; b2=b1;b1=b; }
    return x*b1-b2+c[0];
}
#endif
static const double trx_oh_trx_model_offset_a_c[30]={-232.399680398,153.649305252,-56.7606534091,23.5448649658,-10.6578480114,4.79767127404,-81.056751598,22.7077837039,-2.96244673295,0.299532376803,0.0532892400568,-0.0105212089343,-39.1668372957,14.9894597998,-2.21767366486,0.405478971463,-0.0117738691504,0.138342962517,-15.9882689259,7.39288842873,-1.35951208431,0.190096146761,0.00637058241878,-0.116429618127,-5.5138140884,3.0706297243,-0.547032748071,0.15553066531,0.0144368154093,-0.0443455945788};
static const double trx_oh_trx_model_offset_b_c[13]={-357.643862343,-265.373321533,59.3405978631,-17.7956076723,5.97881763422,-2.11982610249,0.784178870543,-0.267558388544,0.0889657720231,0.0650633677486,0.00493977326511,-0.0123225741715,-0.021290158449};
static inline int trx_model_offset(unsigned i) {
    if(i>=128) return (int)trx_model_round(trx_oh_trx_model_cheb(trx_oh_trx_model_offset_b_c,13,2.0*(i-128)/127.0-1.0));
    unsigned part=i<8?0:i<16?1:i<32?2:i<64?3:4;
    static const unsigned trx_oh_bounds[]={0,8,16,32,64,128};
    unsigned lo=trx_oh_bounds[part],hi=trx_oh_bounds[part+1];
    return (int)trx_model_round(trx_oh_trx_model_cheb(trx_oh_trx_model_offset_a_c+part*6,6,2.0*(i-lo)/(hi-lo-1)-1.0));
}
static inline uint32_t trx_oh_trx_model_memory(unsigned i) {
    if(i<256) return (uint32_t)trx_model_offset(i)&0xffffffu;
    unsigned x=i<4352?i-256:i-4352;
    double t=trx_oh_trx_model_exp(0.003372927270490704*x+(-12.768360569597025));
    return trx_oh_trx_model_word(5931641.6632877039*(i<4352?trx_oh_trx_model_cos(t):trx_oh_trx_model_exp(-t)));
}



static uint32_t trx_oh_metal_norm(unsigned i){return trx_oh_trx_model_memory(i);}
static uint32_t trx_oh_metal_filter(unsigned i){return trx_oh_trx_model_memory(i);}
static uint32_t trx_oh_metal_offsets(unsigned i){return (uint32_t)trx_model_offset(i)&0xffffffu;}
static uint32_t trx_oh_metal_square(unsigned source,unsigned index){
 static const unsigned trx_oh_transition[6]={72,51,43,40,29,26};
 return index<trx_oh_transition[source]?8387769u:((uint32_t)-8387769)&0xffffffu;
}






static inline void trx_oh_metal_source(uint32_t *w,unsigned start,uint32_t cursors[6],const uint32_t gains[6],int add)
{

 static const unsigned trx_oh_periods[6]={114,102,86,80,58,52};
 for(unsigned source=0;source<6;++source){
  for(unsigned i=0;i<32;++i){
   uint64_t product=trx_oh_md_mpy(trx_oh_metal_square(source,cursors[source]),gains[source]);
   if(source||add)product=trx_oh_md_add(product,trx_oh_md_from24(w[start+i]));
   w[start+i]=trx_oh_md_acc_to_word(product);
   if(++cursors[source]==trx_oh_periods[source])cursors[source]=0;
  }
 }
}
static inline void trx_oh_metal_coeff(unsigned index,unsigned resonance,uint32_t *feedback,uint32_t *square,uint32_t *feed)
{
 uint32_t pole=trx_oh_metal_filter(0x1900+index+trx_oh_md_s24(trx_oh_metal_offsets(128+resonance)));
 uint64_t product=trx_oh_md_mpy(pole,trx_oh_metal_norm(0x900+index+trx_oh_md_s24(trx_oh_metal_offsets(resonance))));
 *square=trx_oh_md_acc_to_word(trx_oh_md_sub(0,trx_oh_md_mpy(pole,pole)));
 *feedback=trx_oh_md_acc_to_word(trx_oh_md_asl(product,1));
 *feed=trx_oh_md_acc_to_word(trx_oh_md_sub(trx_oh_md_from24(0x400000),trx_oh_md_add(trx_oh_md_asl(product,1),trx_oh_md_from24(*square))));
}

static inline void trx_oh_metal_resonant_lowpass(uint32_t *w,unsigned input_base,unsigned output_base,uint32_t history[4],unsigned index,unsigned resonance)
{
 uint32_t feedback,square,feed;trx_oh_metal_coeff(index,resonance,&feedback,&square,&feed);
 w[0]=history[0];w[1]=history[1];
 w[input_base-2]=history[2];w[input_base-1]=history[3];
 unsigned history_pos=0,input=input_base+1,delay=input_base-2,adjacent=input_base-1;
 uint64_t first=trx_oh_md_mac(trx_oh_md_mpy(square,w[0]),feedback,w[1]);
 first=trx_oh_md_mac(first,w[input_base],feed);
 for(unsigned i=0;i<32;++i){
  first=trx_oh_md_asl(first,1);
  uint64_t second=trx_oh_md_mpy(square,w[delay++]);
  second=trx_oh_md_mac(second,feedback,w[adjacent++]);
  uint32_t first_word=trx_oh_md_acc_to_word(first);
  second=trx_oh_md_mac(second,first_word,feed);
  w[history_pos]=first_word;history_pos^=1;
  second=trx_oh_md_asl(second,1);
  first=trx_oh_md_mpy(square,w[history_pos]);history_pos^=1;
  first=trx_oh_md_mac(first,feedback,w[history_pos]);history_pos^=1;
  first=trx_oh_md_mac(first,w[input--],feed);
  w[input]=trx_oh_md_acc_to_word(second);input+=2;
 }

 (void)output_base;
 history[0]=w[history_pos];history_pos^=1;history[1]=w[history_pos];
 history[3]=w[adjacent];history[2]=w[adjacent-1];
}

static inline void trx_oh_metal_difference_filter(uint32_t *w,unsigned input_base,unsigned output_base,uint32_t history[4],unsigned index,unsigned resonance)
{
 uint32_t feedback,square,unused;trx_oh_metal_coeff(index,resonance,&feedback,&square,&unused);
 w[input_base-2]=history[0];w[input_base-1]=history[1];
 w[output_base-2]=history[2];w[output_base-1]=history[3];
 unsigned input=input_base-2,prior=output_base-2,output=output_base-1;
 uint64_t previous=trx_oh_md_from24(w[prior+1]);
 uint32_t sample=w[input++];
 for(unsigned i=0;i<32;++i){
  uint64_t filtered=trx_oh_md_mpy(0x400000,sample); sample=w[input++];
  filtered=trx_oh_md_mac(filtered,0x800000,sample);sample=w[input--];
  filtered=trx_oh_md_mac(filtered,0x400000,sample);sample=w[prior++];
  filtered=trx_oh_md_mac(filtered,square,sample);sample=trx_oh_md_acc_to_word(previous);
  filtered=trx_oh_md_mac(filtered,feedback,sample);sample=w[input++];
  filtered=trx_oh_md_asl(filtered,1);
  w[output++]=trx_oh_md_acc_to_word(previous);previous=filtered;
 }
 history[2]=w[prior];history[3]=trx_oh_md_acc_to_word(previous);w[output]=history[3];
 history[1]=w[input];history[0]=w[input-1];
}









static void trx_oh_clear_state(void *memory,unsigned bytes)
{
 volatile unsigned char *cursor=memory;
 while(bytes--)*cursor++=0;
}
static void trx_oh_hat_controls(mdkit_trx_oh *s)
{
 uint32_t raw[8],p[64];for(unsigned i=0;i<8;++i)raw[i]=(uint32_t)(128*s->clock.current[i]);
 trx_oh_oh_convert(raw,p);
 s->linear_step=p[0];s->decay=p[1];s->lowpass_index=p[2];s->lowpass_resonance=p[3];
 s->noise_gain=p[4];s->highpass_index=p[5];s->highpass_resonance=p[6];s->metal_gain=p[7];s->hold_blocks=p[8];s->fade_blocks=p[9];
}

static void trx_oh_hat_reset_sources(mdkit_trx_oh *s)
{
 s->noise_a=0;s->noise_b=1;trx_oh_clear_state(s->cursors,sizeof s->cursors);
}
static void trx_oh_hat_init_kind(void *state,unsigned machine)
{
 mdkit_trx_oh *s=state;trx_oh_clear_state(s,sizeof *s);s->machine=machine;

 trx_oh_md_control_clock_seed(&s->clock,8,mdkit_trx_oh_defaults);s->clock.next_update=2;
 for(unsigned i=0;i<8;++i){s->queued_targets[i]=mdkit_trx_oh_defaults[i];s->last_raw[i]=(unsigned)mdkit_trx_oh_defaults[i] * 128u;}
 trx_oh_hat_controls(s);trx_oh_hat_reset_sources(s);
}

static void trx_oh_open_init(void *state){trx_oh_hat_init_kind(state,23);}
static void trx_oh_hat_set(void *state,unsigned index,unsigned value)
{mdkit_trx_oh*s=state;if(index>=5)return;s->queued_targets[index]=value>127?127:value;s->target_mask|=1u<<index;}
static void trx_oh_hat_trigger(void *state){((mdkit_trx_oh*)state)->pending_trigger=1;}
static void trx_oh_hat_boundary(mdkit_trx_oh *s)
{
 mdkit_control_clock*c=&s->clock;int publish=0;
 if(c->pending_valid&&c->block>=c->publish_block){for(unsigned i=0;i<8;++i)c->current[i]=c->pending[i];c->pending_valid=0;publish=1;}
 if(c->block==c->next_update){int changed=0;for(unsigned i=0;i<8;++i){c->pending[i]=c->raw[i];uint32_t raw=(uint32_t)(128*c->raw[i]);changed|=raw!=s->last_raw[i];s->last_raw[i]=raw;c->raw[i]=(float)((3*raw+128*(uint32_t)c->target[i])/4)/128;}
 c->pending_valid=changed;c->publish_block=c->block+1;c->next_update+=12;}
 for(unsigned i=0;i<8;++i)if(s->target_mask&(1u<<i))c->target[i]=(float)s->queued_targets[i];s->target_mask=0;
 if(s->pending_trigger){for(unsigned i=0;i<8;++i)c->current[i]=c->raw[i];publish=1;}
 if(publish){uint32_t hold=s->hold_blocks,fade=s->fade_blocks;trx_oh_hat_controls(s);if(!s->pending_trigger){s->hold_blocks=hold;s->fade_blocks=fade;}}
 if(s->pending_trigger){if(!s->triggered){trx_oh_hat_reset_sources(s);s->amplitude=0;}
 s->latched_metal_gain=s->metal_gain;
 s->amplitude=trx_oh_md_acc_to_word(trx_oh_md_add(trx_oh_md_from24(0x6ccccd),trx_oh_md_from24(s->amplitude)));
 trx_oh_clear_state(s->resonant_history,sizeof s->resonant_history);trx_oh_clear_state(s->difference_history,sizeof s->difference_history);
 s->pending_trigger=0;s->triggered=1;}
 ++c->block;
}
static void trx_oh_hat_block(mdkit_trx_oh*s)
{
 uint32_t*w=s->workspace,a=s->noise_a,b=s->noise_b;
 for(unsigned i=0;i<32;++i){uint32_t sum=(a+b)&trx_oh_MD_MASK24;w[32+i]=trx_oh_md_acc_to_word(trx_oh_md_mpy(sum,s->noise_gain));a=b;b=sum;}
 s->noise_a=a;s->noise_b=b;
 uint32_t gains[6];for(unsigned i=0;i<6;++i)gains[i]=i?s->latched_metal_gain:s->metal_gain;
 trx_oh_metal_source(w,32,s->cursors,gains,1);
 trx_oh_metal_resonant_lowpass(w,32,32,s->resonant_history,s->lowpass_index,s->lowpass_resonance);
 trx_oh_metal_difference_filter(w,32,96,s->difference_history[0],s->highpass_index,s->highpass_resonance);
 trx_oh_metal_difference_filter(w,96,32,s->difference_history[1],0x30,0);
 uint64_t envelope=trx_oh_md_from24(s->amplitude);
 int mode=s->hold_blocks?0:s->fade_blocks?1:2;
 if(mode==0)--s->hold_blocks;else if(mode==1)--s->fade_blocks;
 uint32_t gain=s->amplitude;
 for(unsigned i=0;i<32;++i){
  if(mode==1)envelope=trx_oh_md_sub(envelope,trx_oh_md_from24(s->linear_step));
  if(mode==2)envelope=trx_oh_md_mpy(s->decay,gain);
  s->block[i]=trx_oh_md_acc_to_word(trx_oh_md_asr(trx_oh_md_mpy(gain,w[32+i]),1));
  if(mode)gain=trx_oh_md_acc_to_word(envelope);
 }
 if(mode)s->amplitude=trx_oh_md_acc_to_word(envelope);
}
static void trx_oh_hat_render(void*state,float*out,unsigned frames)
{mdkit_trx_oh*s=state;while(frames){if(!s->pos){trx_oh_hat_boundary(s);trx_oh_hat_block(s);}unsigned n=32-s->pos;if(n>frames)n=frames;for(unsigned i=0;i<n;++i)out[i]=s->triggered?((float)trx_oh_md_s24(s->block[s->pos+i])/8388608):0.0f;frames-=n;out+=n;s->pos=(s->pos+n)&31;}}
static const char*const trx_oh_names[]={"GAP","DEC","HPF","LPF","MTAL"};

const mdkit_voice mdkit_voice_trx_oh={"TRX-OH",23,5,trx_oh_names,sizeof(mdkit_trx_oh),trx_oh_open_init,trx_oh_hat_set,trx_oh_hat_trigger,trx_oh_hat_render, mdkit_trx_oh_defaults
};
