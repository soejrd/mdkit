/* SPDX-License-Identifier: CC0-1.0
 * TRX-BD standalone synthesis voice.
 */
#include "mdkit_trx_bd.h"

/* ---- Default controls --------------------------------------------------- */
/* Active controls are in descriptor order; unused clock slots stay at 64. */
static const unsigned char mdkit_trx_bd_defaults[MDKIT_MAX_CONTROLS] = {
    64, 64, 0, 0, 0, 0, 0, 0
};
#define trx_bd_MD_MASK24 UINT32_C(0xffffff)
#define trx_bd_MD_MASK48 UINT64_C(0xffffffffffff)
#define trx_bd_MD_MASK56 UINT64_C(0xffffffffffffff)
#define trx_bd_MD_VOICE_BLOCK 32u
enum { trx_bd_MD_TRX_PUBLISH=1,trx_bd_MD_TRX_TRIGGER=2,trx_bd_MD_TRX_FIRST=4 };
static inline int64_t trx_bd_md_signed_bits(uint64_t word, unsigned bits)
{
    const uint64_t sign = UINT64_C(1) << (bits - 1);
    const uint64_t mask = (UINT64_C(1) << bits) - 1;
    word &= mask;
    return (word & sign) ? (int64_t)word - (int64_t)(UINT64_C(1) << bits)
                         : (int64_t)word;
}
static inline int64_t trx_bd_md_s24(uint32_t w) { return trx_bd_md_signed_bits(w & trx_bd_MD_MASK24, 24); }
static inline int64_t trx_bd_md_s56(uint64_t a) { return trx_bd_md_signed_bits(a & trx_bd_MD_MASK56, 56); }
static inline uint32_t trx_bd_md_mid(uint64_t a) { return (uint32_t)((a >> 24) & trx_bd_MD_MASK24); }
static inline uint32_t trx_bd_md_low(uint64_t a) { return (uint32_t)(a & trx_bd_MD_MASK24); }
static inline uint64_t trx_bd_md_from24(uint32_t w)
{ return ((uint64_t)(trx_bd_md_s24(w) * (INT64_C(1) << 24))) & trx_bd_MD_MASK56; }
static inline uint64_t trx_bd_md_from48(uint64_t w)
{ return (uint64_t)trx_bd_md_signed_bits(w & trx_bd_MD_MASK48, 48) & trx_bd_MD_MASK56; }
static inline uint64_t trx_bd_md_setmid(uint64_t a, uint32_t w)
{ return (a & ~(UINT64_C(0xffffff) << 24)) | ((uint64_t)(w & trx_bd_MD_MASK24) << 24); }
static inline uint64_t trx_bd_md_add(uint64_t a, uint64_t b) { return (a + b) & trx_bd_MD_MASK56; }
static inline uint64_t trx_bd_md_sub(uint64_t a, uint64_t b) { return (a - b) & trx_bd_MD_MASK56; }
static inline uint64_t trx_bd_md_asl(uint64_t a, unsigned n) { return (a << n) & trx_bd_MD_MASK56; }
static inline uint64_t trx_bd_md_asr(uint64_t a, unsigned n)
{
    const int64_t v = trx_bd_md_s56(a);
    const int64_t sh = v >= 0 ? v / (INT64_C(1) << n)
        : -((-v + (INT64_C(1) << n) - 1) / (INT64_C(1) << n));
    return (uint64_t)sh & trx_bd_MD_MASK56;
}
static inline uint64_t trx_bd_md_mpy(uint32_t a, uint32_t b)
{ return (uint64_t)(trx_bd_md_s24(a) * trx_bd_md_s24(b) * 2) & trx_bd_MD_MASK56; }
static inline uint64_t trx_bd_md_mac(uint64_t acc, uint32_t x, uint32_t y)
{
    return (acc + trx_bd_md_mpy(x, y)) & trx_bd_MD_MASK56;
}
static inline uint64_t trx_bd_md_and_mid(uint64_t a, uint32_t mask)
{ return trx_bd_md_setmid(a, trx_bd_md_mid(a) & mask); }
static inline uint64_t trx_bd_md_pack48(uint32_t hi, uint32_t lo)
{ return ((uint64_t)(hi & trx_bd_MD_MASK24) << 24) | (lo & trx_bd_MD_MASK24); }
static inline uint32_t trx_bd_md_acc_to_word(uint64_t a)
{
    const int64_t v = trx_bd_md_s56(a);
    if(v < -(INT64_C(0x800000) << 24)) return UINT32_C(0x800000);
    if(v >  (INT64_C(0x7fffff) << 24)) return UINT32_C(0x7fffff);
    return trx_bd_md_mid(a);
}
static inline void trx_bd_md_control_clock_seed(mdkit_control_clock *c, unsigned count,
                                         const unsigned char *knobs)
{
    c->count = count;
    for (unsigned i = 0; i < count; ++i) {
        const float v = (float)knobs[i];
        c->target[i] = c->raw[i] = c->current[i] = c->pending[i] = v;
    }
    c->block = 0; c->next_update = 0; c->publish_block = 0; c->pending_valid = 0;
}
static inline void trx_bd_md_trx_events_init(mdkit_control_clock *c,mdkit_trx_bd_events *e)
{

    trx_bd_md_control_clock_seed(c,8,mdkit_trx_bd_defaults); c->next_update=2;
    e->targets=e->trigger=e->triggered=0;
    for(unsigned i=0;i<8;++i){e->queued[i]=mdkit_trx_bd_defaults[i];e->last_raw[i]=(unsigned)mdkit_trx_bd_defaults[i] * 128u;}
}
static inline void trx_bd_md_trx_events_target(mdkit_trx_bd_events *e,unsigned count,unsigned i,unsigned value)
{ if(i<count){e->queued[i]=value>127?127:value;e->targets|=1u<<i;} }
static inline unsigned trx_bd_md_trx_events_boundary(mdkit_control_clock *c,mdkit_trx_bd_events *e,int trigger_only)
{
    unsigned result=0;
    if(c->pending_valid && c->block>=c->publish_block){
        for(unsigned i=0;i<8;++i)c->current[i]=c->pending[i];
        c->pending_valid=0;result=trx_bd_MD_TRX_PUBLISH;
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
        result=trx_bd_MD_TRX_PUBLISH|trx_bd_MD_TRX_TRIGGER|(e->triggered?0:trx_bd_MD_TRX_FIRST);
        e->trigger=0;e->triggered=1;
    }
    ++c->block;return result;
}
static uint32_t trx_bd_local_sine(unsigned index) {
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

static const uint32_t trx_bd_control_word_0[128]={
 0x7dc0f5u,0x7ddb1eu,0x7e0029u,0x7e1778u,0x7e3878u,0x7e4d3bu,0x7e6a9fu,0x7e7d1du,
 0x7e9749u,0x7ea7bfu,0x7ebf0du,0x7ecdb5u,0x7ee274u,0x7eef7fu,0x7f01f7u,0x7f0d93u,
 0x7f1e02u,0x7f2858u,0x7f36f8u,0x7f402au,0x7f4d2eu,0x7f555du,0x7f60f1u,0x7f6839u,
 0x7f7286u,0x7f7901u,0x7f822bu,0x7f87eeu,0x7f9016u,0x7f9536u,0x7f9c77u,0x7fa106u,
 0x7fa77au,0x7fab88u,0x7fb146u,0x7fb4e1u,0x7fb9fcu,0x7fbd32u,0x7fc1bcu,0x7fc497u,
 0x7fc8a0u,0x7fcb2au,0x7fcec2u,0x7fd104u,0x7fd435u,0x7fd637u,0x7fd90fu,0x7fdad8u,
 0x7fdd5eu,0x7fdef5u,0x7fe134u,0x7fe29eu,0x7fe49du,0x7fe5dfu,0x7fe7a6u,0x7fe8c4u,
 0x7fea58u,0x7feb56u,0x7fecbeu,0x7feda0u,0x7feee0u,0x7fefa9u,0x7ff0c6u,0x7ff178u,
 0x7ff275u,0x7ff314u,0x7ff3f5u,0x7ff483u,0x7ff54bu,0x7ff5c9u,0x7ff67au,0x7ff6eau,
 0x7ff789u,0x7ff7ecu,0x7ff879u,0x7ff8d1u,0x7ff94eu,0x7ff99du,0x7ffa0cu,0x7ffa52u,
 0x7ffab5u,0x7ffaf3u,0x7ffb4bu,0x7ffb82u,0x7ffbd0u,0x7ffc02u,0x7ffc47u,0x7ffc73u,
 0x7ffcb1u,0x7ffcd8u,0x7ffd0fu,0x7ffd31u,0x7ffd62u,0x7ffd81u,0x7ffdacu,0x7ffdc8u,
 0x7ffdeeu,0x7ffe07u,0x7ffe29u,0x7ffe3fu,0x7ffe5du,0x7ffe70u,0x7ffe8cu,0x7ffe9du,
 0x7ffeb5u,0x7ffec4u,0x7ffed9u,0x7ffee7u,0x7ffefau,0x7fff06u,0x7fff17u,0x7fff22u,
 0x7fff31u,0x7fff3au,0x7fff48u,0x7fff50u,0x7fff5cu,0x7fff64u,0x7fff6eu,0x7fff75u,
 0x7fff7fu,0x7fff84u,0x7fff8du,0x7fff92u,0x7fff9au,0x7fff9eu,0x7fffa5u,0x7fffa9u,
};
static const uint32_t trx_bd_control_word_1[128]={
 0x000000u,0x010000u,0x020000u,0x030000u,0x040000u,0x050000u,0x060000u,0x070000u,
 0x080000u,0x090000u,0x0a0000u,0x0b0000u,0x0c0000u,0x0d0000u,0x0e0000u,0x0f0000u,
 0x100000u,0x110000u,0x120000u,0x130000u,0x140000u,0x150000u,0x160000u,0x170000u,
 0x180000u,0x190000u,0x1a0000u,0x1b0000u,0x1c0000u,0x1d0000u,0x1e0000u,0x1f0000u,
 0x200000u,0x210000u,0x220000u,0x230000u,0x240000u,0x250000u,0x260000u,0x270000u,
 0x280000u,0x290000u,0x2a0000u,0x2b0000u,0x2c0000u,0x2d0000u,0x2e0000u,0x2f0000u,
 0x300000u,0x310000u,0x320000u,0x330000u,0x340000u,0x350000u,0x360000u,0x370000u,
 0x380000u,0x390000u,0x3a0000u,0x3b0000u,0x3c0000u,0x3d0000u,0x3e0000u,0x3f0000u,
 0x400000u,0x410000u,0x420000u,0x430000u,0x440000u,0x450000u,0x460000u,0x470000u,
 0x480000u,0x490000u,0x4a0000u,0x4b0000u,0x4c0000u,0x4d0000u,0x4e0000u,0x4f0000u,
 0x500000u,0x510000u,0x520000u,0x530000u,0x540000u,0x550000u,0x560000u,0x570000u,
 0x580000u,0x590000u,0x5a0000u,0x5b0000u,0x5c0000u,0x5d0000u,0x5e0000u,0x5f0000u,
 0x600000u,0x610000u,0x620000u,0x630000u,0x640000u,0x650000u,0x660000u,0x670000u,
 0x680000u,0x690000u,0x6a0000u,0x6b0000u,0x6c0000u,0x6d0000u,0x6e0000u,0x6f0000u,
 0x700000u,0x710000u,0x720000u,0x730000u,0x740000u,0x750000u,0x760000u,0x770000u,
 0x780000u,0x790000u,0x7a0000u,0x7b0000u,0x7c0000u,0x7d0000u,0x7e0000u,0x7f0000u,
};
static const uint32_t trx_bd_control_word_2[128]={
 0x0005aau,0x0005bau,0x0005cau,0x0005dau,0x0005eau,0x0005fbu,0x00060cu,0x00061du,
 0x00062eu,0x00063fu,0x000650u,0x000662u,0x000674u,0x000686u,0x000698u,0x0006abu,
 0x0006bdu,0x0006d0u,0x0006e3u,0x0006f7u,0x00070au,0x00071eu,0x000732u,0x000746u,
 0x00075au,0x00076fu,0x000784u,0x000799u,0x0007aeu,0x0007c3u,0x0007d9u,0x0007efu,
 0x000800u,0x000816u,0x00082du,0x000844u,0x00085bu,0x000872u,0x00088au,0x0008a2u,
 0x0008bau,0x0008d3u,0x0008ebu,0x000904u,0x00091du,0x000937u,0x000951u,0x00096bu,
 0x000985u,0x0009a0u,0x0009bbu,0x0009d6u,0x0009f1u,0x000a0du,0x000a29u,0x000a46u,
 0x000a63u,0x000a80u,0x000a9du,0x000abbu,0x000ad9u,0x000af7u,0x000b16u,0x000b35u,
 0x000b54u,0x000b74u,0x000b94u,0x000bb4u,0x000bd5u,0x000bf6u,0x000c18u,0x000c3au,
 0x000c5cu,0x000c7eu,0x000ca1u,0x000cc5u,0x000ce8u,0x000d0du,0x000d31u,0x000d56u,
 0x000d7bu,0x000da1u,0x000dc7u,0x000deeu,0x000e15u,0x000e3cu,0x000e64u,0x000e8cu,
 0x000eb5u,0x000edeu,0x000f08u,0x000f32u,0x000f5cu,0x000f87u,0x000fb3u,0x000fdfu,
 0x001000u,0x00102cu,0x00105au,0x001088u,0x0010b6u,0x0010e4u,0x001114u,0x001144u,
 0x001174u,0x0011a6u,0x0011d6u,0x001208u,0x00123au,0x00126eu,0x0012a2u,0x0012d6u,
 0x00130au,0x001340u,0x001376u,0x0013acu,0x0013e2u,0x00141au,0x001452u,0x00148cu,
 0x0014c6u,0x001500u,0x00153au,0x001576u,0x0015b2u,0x0015eeu,0x00162cu,0x00166au,
};
static const uint32_t trx_bd_control_word_3[128]={
 0x000003u,0x000003u,0x000003u,0x000003u,0x000003u,0x000003u,0x000003u,0x000003u,
 0x000003u,0x000003u,0x000003u,0x000003u,0x000003u,0x000004u,0x000004u,0x000004u,
 0x000005u,0x000005u,0x000005u,0x000006u,0x000006u,0x000007u,0x000008u,0x000008u,
 0x000009u,0x00000au,0x00000bu,0x00000cu,0x00000du,0x00000eu,0x000010u,0x000011u,
 0x000013u,0x000014u,0x000016u,0x000017u,0x000019u,0x00001bu,0x00001du,0x00001fu,
 0x000022u,0x000024u,0x000027u,0x000029u,0x00002cu,0x00002fu,0x000032u,0x000035u,
 0x000039u,0x00003cu,0x00003fu,0x000043u,0x000047u,0x00004bu,0x00004fu,0x000054u,
 0x000058u,0x00005du,0x000062u,0x000067u,0x00006cu,0x000071u,0x000077u,0x00007du,
 0x000083u,0x000089u,0x00008fu,0x000095u,0x00009cu,0x0000a3u,0x0000aau,0x0000b1u,
 0x0000b9u,0x0000c0u,0x0000c8u,0x0000d0u,0x0000d9u,0x0000e1u,0x0000eau,0x0000f3u,
 0x0000fdu,0x000106u,0x000110u,0x000119u,0x000124u,0x00012eu,0x000139u,0x000144u,
 0x00014fu,0x00015bu,0x000166u,0x000172u,0x00017fu,0x00018bu,0x000198u,0x0001a5u,
 0x0001b3u,0x0001c0u,0x0001ceu,0x0001dcu,0x0001ebu,0x0001f9u,0x000208u,0x000218u,
 0x000228u,0x000238u,0x000248u,0x000258u,0x00026au,0x00027au,0x00028cu,0x00029eu,
 0x0002b1u,0x0002c3u,0x0002d6u,0x0002e9u,0x0002fdu,0x000310u,0x000325u,0x000339u,
 0x00034eu,0x000363u,0x000379u,0x00038fu,0x0003a5u,0x0003bcu,0x0003d3u,0x0003ebu,
};
static const uint32_t trx_bd_control_word_4[128]={
 0x5f29c2u,0x60775cu,0x61b9eau,0x62f199u,0x641e9au,0x654120u,0x66e1bfu,0x67eb0cu,
 0x68eaabu,0x69e0d9u,0x6acdd6u,0x6c20a2u,0x6cf7beu,0x6dc68au,0x6e8d46u,0x6f4c34u,
 0x705c80u,0x7108fdu,0x71ae88u,0x724d5cu,0x72e5b6u,0x7377cfu,0x7447bau,0x74cb2bu,
 0x75491eu,0x75c1c9u,0x76355eu,0x76d9a2u,0x774158u,0x77a4a0u,0x7803a7u,0x785e98u,
 0x78dfb4u,0x793123u,0x797f08u,0x79c98au,0x7a10cau,0x7a54edu,0x7ab591u,0x7af277u,
 0x7b2caeu,0x7b6453u,0x7b9982u,0x7be4e5u,0x7c1460u,0x7c41beu,0x7c6d17u,0x7c9680u,
 0x7cd12du,0x7cf61du,0x7d1965u,0x7d3b18u,0x7d5b48u,0x7d7a06u,0x7da58fu,0x7dc0f5u,
 0x7ddb1eu,0x7df419u,0x7e0bf3u,0x7e2db9u,0x7e42f9u,0x7e5742u,0x7e6a9fu,0x7e7d1du,
 0x7e9749u,0x7ea7bfu,0x7eb776u,0x7ec677u,0x7ed4c8u,0x7ee274u,0x7ef5ccu,0x7f01f7u,
 0x7f0d93u,0x7f18a9u,0x7f233du,0x7f3235u,0x7f3b9fu,0x7f449bu,0x7f4d2eu,0x7f555du,
 0x7f60f1u,0x7f6839u,0x7f6f2cu,0x7f75cdu,0x7f7c21u,0x7f822bu,0x7f8ab6u,0x7f9016u,
 0x7f9536u,0x7f9a1au,0x7f9ec5u,0x7fa560u,0x7fa987u,0x7fad7eu,0x7fb146u,0x7fb4e1u,
 0x7fb9fcu,0x7fbd32u,0x7fc042u,0x7fc32eu,0x7fc5f7u,0x7fc8a0u,0x7fcc64u,0x7fcec2u,
 0x7fd104u,0x7fd32bu,0x7fd539u,0x7fd822u,0x7fd9f6u,0x7fdbb4u,0x7fdd5eu,0x7fdef5u,
 0x7fe134u,0x7fe29eu,0x7fe3f7u,0x7fe540u,0x7fe67au,0x7fe7a6u,0x7fe94eu,0x7fea58u,
 0x7feb56u,0x7fec49u,0x7fed30u,0x7fee78u,0x7fef46u,0x7ff00au,0x7ff0c6u,0x7ff178u,
};
static const uint32_t trx_bd_control_word_5[128]={
 0x7fffffu,0x7fffffu,0x7fffffu,0x7fffffu,0x7fffffu,0x7fffffu,0x7fffffu,0x7fffffu,
 0x7fffffu,0x7fffffu,0x7fffffu,0x7fffffu,0x7fffffu,0x7fffffu,0x7fffffu,0x7fffffu,
 0x7fffffu,0x7fffffu,0x7fffffu,0x7fffffu,0x7fffffu,0x7fffffu,0x7fffffu,0x7fffffu,
 0x7fffffu,0x7fffffu,0x7fffffu,0x7fffffu,0x7fffffu,0x7fffffu,0x7fffffu,0x7fffffu,
 0x7fffffu,0x7fffffu,0x7fffffu,0x7fffffu,0x7fffffu,0x7fffffu,0x7fffffu,0x7fffffu,
 0x7fffffu,0x7fffffu,0x7fffffu,0x7fffffu,0x7fffffu,0x7fffffu,0x7fffffu,0x7fffffu,
 0x7fffffu,0x7fffffu,0x7fffffu,0x7fffffu,0x7fffffu,0x7fffffu,0x7fffffu,0x7fffffu,
 0x7fffffu,0x7fffffu,0x7fffffu,0x7fffffu,0x7fffffu,0x7fffffu,0x7fffffu,0x7fffffu,
 0x0fffffu,0x0fcdffu,0x0f9bffu,0x0f69ffu,0x0f37ffu,0x0f05ffu,0x0ed3ffu,0x0ea1ffu,
 0x0e6fffu,0x0e3dffu,0x0e0bffu,0x0dd9ffu,0x0da7ffu,0x0d75ffu,0x0d43ffu,0x0d11ffu,
 0x0cdfffu,0x0cadffu,0x0c7bffu,0x0c49ffu,0x0c17ffu,0x0be5ffu,0x0bb3ffu,0x0b81ffu,
 0x0b4fffu,0x0b1dffu,0x0aebffu,0x0ab9ffu,0x0a87ffu,0x0a55ffu,0x0a23ffu,0x09f1ffu,
 0x09bfffu,0x098dffu,0x095bffu,0x0929ffu,0x08f7ffu,0x08c5ffu,0x0893ffu,0x0861ffu,
 0x082fffu,0x07fdffu,0x07cbffu,0x0799ffu,0x0767ffu,0x0735ffu,0x0703ffu,0x06d1ffu,
 0x069fffu,0x066dffu,0x063bffu,0x0609ffu,0x05d7ffu,0x05a5ffu,0x0573ffu,0x0541ffu,
 0x050fffu,0x04ddffu,0x04abffu,0x0479ffu,0x0447ffu,0x0415ffu,0x03e3ffu,0x03b1ffu,
};
static const uint32_t trx_bd_control_word_6[128]={
 0x800000u,0x800000u,0x800000u,0x800000u,0x800000u,0x800000u,0x800000u,0x800000u,
 0x800000u,0x800000u,0x800000u,0x800000u,0x800000u,0x800000u,0x800000u,0x800000u,
 0x800000u,0x800000u,0x800000u,0x800000u,0x800000u,0x800000u,0x800000u,0x800000u,
 0x800000u,0x800000u,0x800000u,0x800000u,0x800000u,0x800000u,0x800000u,0x800000u,
 0x800000u,0x800000u,0x800000u,0x800000u,0x800000u,0x800000u,0x800000u,0x800000u,
 0x800000u,0x800000u,0x800000u,0x800000u,0x800000u,0x800000u,0x800000u,0x800000u,
 0x800000u,0x800000u,0x800000u,0x800000u,0x800000u,0x800000u,0x800000u,0x800000u,
 0x800000u,0x800000u,0x800000u,0x800000u,0x800000u,0x800000u,0x800000u,0x800000u,
 0xf00000u,0xf03200u,0xf06400u,0xf09600u,0xf0c800u,0xf0fa00u,0xf12c00u,0xf15e00u,
 0xf19000u,0xf1c200u,0xf1f400u,0xf22600u,0xf25800u,0xf28a00u,0xf2bc00u,0xf2ee00u,
 0xf32000u,0xf35200u,0xf38400u,0xf3b600u,0xf3e800u,0xf41a00u,0xf44c00u,0xf47e00u,
 0xf4b000u,0xf4e200u,0xf51400u,0xf54600u,0xf57800u,0xf5aa00u,0xf5dc00u,0xf60e00u,
 0xf64000u,0xf67200u,0xf6a400u,0xf6d600u,0xf70800u,0xf73a00u,0xf76c00u,0xf79e00u,
 0xf7d000u,0xf80200u,0xf83400u,0xf86600u,0xf89800u,0xf8ca00u,0xf8fc00u,0xf92e00u,
 0xf96000u,0xf99200u,0xf9c400u,0xf9f600u,0xfa2800u,0xfa5a00u,0xfa8c00u,0xfabe00u,
 0xfaf000u,0xfb2200u,0xfb5400u,0xfb8600u,0xfbb800u,0xfbea00u,0xfc1c00u,0xfc4e00u,
};
static const uint32_t trx_bd_control_word_7[128]={
 0x1fffffu,0x1fc17fu,0x1f82ffu,0x1f447fu,0x1f05ffu,0x1ec77fu,0x1e88ffu,0x1e4a7fu,
 0x1e0bffu,0x1dcd7fu,0x1d8effu,0x1d507fu,0x1d11ffu,0x1cd37fu,0x1c94ffu,0x1c567fu,
 0x1c17ffu,0x1bd97fu,0x1b9affu,0x1b5c7fu,0x1b1dffu,0x1adf7fu,0x1aa0ffu,0x1a627fu,
 0x1a23ffu,0x19e57fu,0x19a6ffu,0x19687fu,0x1929ffu,0x18eb7fu,0x18acffu,0x186e7fu,
 0x182fffu,0x17f17fu,0x17b2ffu,0x17747fu,0x1735ffu,0x16f77fu,0x16b8ffu,0x167a7fu,
 0x163bffu,0x15fd7fu,0x15beffu,0x15807fu,0x1541ffu,0x15037fu,0x14c4ffu,0x14867fu,
 0x1447ffu,0x14097fu,0x13caffu,0x138c7fu,0x134dffu,0x130f7fu,0x12d0ffu,0x12927fu,
 0x1253ffu,0x12157fu,0x11d6ffu,0x11987fu,0x1159ffu,0x111b7fu,0x10dcffu,0x109e7fu,
 0x105fffu,0x10217fu,0x0fe2ffu,0x0fa47fu,0x0f65ffu,0x0f277fu,0x0ee8ffu,0x0eaa7fu,
 0x0e6bffu,0x0e2d7fu,0x0deeffu,0x0db07fu,0x0d71ffu,0x0d337fu,0x0cf4ffu,0x0cb67fu,
 0x0c77ffu,0x0c397fu,0x0bfaffu,0x0bbc7fu,0x0b7dffu,0x0b3f7fu,0x0b00ffu,0x0ac27fu,
 0x0a83ffu,0x0a457fu,0x0a06ffu,0x09c87fu,0x0989ffu,0x094b7fu,0x090cffu,0x08ce7fu,
 0x088fffu,0x08517fu,0x0812ffu,0x07d47fu,0x0795ffu,0x07577fu,0x0718ffu,0x06da7fu,
 0x069bffu,0x065d7fu,0x061effu,0x05e07fu,0x05a1ffu,0x05637fu,0x0524ffu,0x04e67fu,
 0x04a7ffu,0x04697fu,0x042affu,0x03ec7fu,0x03adffu,0x036f7fu,0x0330ffu,0x02f27fu,
 0x02b3ffu,0x02757fu,0x0236ffu,0x01f87fu,0x01b9ffu,0x017b7fu,0x013cffu,0x00fe7fu,
};
static const uint32_t trx_bd_control_word_8[128]={
 0xe00000u,0xe03e80u,0xe07d00u,0xe0bb80u,0xe0fa00u,0xe13880u,0xe17700u,0xe1b580u,
 0xe1f400u,0xe23280u,0xe27100u,0xe2af80u,0xe2ee00u,0xe32c80u,0xe36b00u,0xe3a980u,
 0xe3e800u,0xe42680u,0xe46500u,0xe4a380u,0xe4e200u,0xe52080u,0xe55f00u,0xe59d80u,
 0xe5dc00u,0xe61a80u,0xe65900u,0xe69780u,0xe6d600u,0xe71480u,0xe75300u,0xe79180u,
 0xe7d000u,0xe80e80u,0xe84d00u,0xe88b80u,0xe8ca00u,0xe90880u,0xe94700u,0xe98580u,
 0xe9c400u,0xea0280u,0xea4100u,0xea7f80u,0xeabe00u,0xeafc80u,0xeb3b00u,0xeb7980u,
 0xebb800u,0xebf680u,0xec3500u,0xec7380u,0xecb200u,0xecf080u,0xed2f00u,0xed6d80u,
 0xedac00u,0xedea80u,0xee2900u,0xee6780u,0xeea600u,0xeee480u,0xef2300u,0xef6180u,
 0xefa000u,0xefde80u,0xf01d00u,0xf05b80u,0xf09a00u,0xf0d880u,0xf11700u,0xf15580u,
 0xf19400u,0xf1d280u,0xf21100u,0xf24f80u,0xf28e00u,0xf2cc80u,0xf30b00u,0xf34980u,
 0xf38800u,0xf3c680u,0xf40500u,0xf44380u,0xf48200u,0xf4c080u,0xf4ff00u,0xf53d80u,
 0xf57c00u,0xf5ba80u,0xf5f900u,0xf63780u,0xf67600u,0xf6b480u,0xf6f300u,0xf73180u,
 0xf77000u,0xf7ae80u,0xf7ed00u,0xf82b80u,0xf86a00u,0xf8a880u,0xf8e700u,0xf92580u,
 0xf96400u,0xf9a280u,0xf9e100u,0xfa1f80u,0xfa5e00u,0xfa9c80u,0xfadb00u,0xfb1980u,
 0xfb5800u,0xfb9680u,0xfbd500u,0xfc1380u,0xfc5200u,0xfc9080u,0xfccf00u,0xfd0d80u,
 0xfd4c00u,0xfd8a80u,0xfdc900u,0xfe0780u,0xfe4600u,0xfe8480u,0xfec300u,0xff0180u,
};
static const uint32_t trx_bd_control_word_9[128]={
 0x000000u,0x0001ffu,0x0007fcu,0x0011f7u,0x001ff0u,0x0031e7u,0x0047dcu,0x0061cfu,
 0x007fc0u,0x00a1afu,0x00c79cu,0x00f187u,0x011f70u,0x015157u,0x01873cu,0x01c11fu,
 0x01ff00u,0x0240dfu,0x0286bcu,0x02d097u,0x031e70u,0x037047u,0x03c61cu,0x041fefu,
 0x047dc0u,0x04df8fu,0x05455cu,0x05af27u,0x061cf0u,0x068eb7u,0x07047cu,0x077e3fu,
 0x07fc00u,0x087dbfu,0x09037cu,0x098d37u,0x0a1af0u,0x0aaca7u,0x0b425cu,0x0bdc0fu,
 0x0c79c0u,0x0d1b6fu,0x0dc11cu,0x0e6ac7u,0x0f1870u,0x0fca17u,0x107fbcu,0x11395fu,
 0x11f700u,0x12b89fu,0x137e3cu,0x1447d7u,0x151570u,0x15e707u,0x16bc9cu,0x17962fu,
 0x1873c0u,0x19554fu,0x1a3adcu,0x1b2467u,0x1c11f0u,0x1d0377u,0x1df8fcu,0x1ef27fu,
 0x1ff000u,0x20f17fu,0x21f6fcu,0x230077u,0x240df0u,0x251f67u,0x2634dcu,0x274e4fu,
 0x286bc0u,0x298d2fu,0x2ab29cu,0x2bdc07u,0x2d0970u,0x2e3ad7u,0x2f703cu,0x30a99fu,
 0x31e700u,0x33285fu,0x346dbcu,0x35b717u,0x370470u,0x3855c7u,0x39ab1cu,0x3b046fu,
 0x3c61c0u,0x3dc30fu,0x3f285cu,0x4091a7u,0x41fef0u,0x437037u,0x44e57cu,0x465ebfu,
 0x47dc00u,0x495d3fu,0x4ae27cu,0x4c6bb7u,0x4df8f0u,0x4f8a27u,0x511f5cu,0x52b88fu,
 0x5455c0u,0x55f6efu,0x579c1cu,0x594547u,0x5af270u,0x5ca397u,0x5e58bcu,0x6011dfu,
 0x61cf00u,0x63901fu,0x65553cu,0x671e57u,0x68eb70u,0x6abc87u,0x6c919cu,0x6e6aafu,
 0x7047c0u,0x7228cfu,0x740ddcu,0x75f6e7u,0x77e3f0u,0x79d4f7u,0x7bc9fcu,0x7dc2ffu,
};
static const uint32_t trx_bd_control_word_10[128]={
 0x000000u,0x008000u,0x010000u,0x018000u,0x020000u,0x028000u,0x030000u,0x038000u,
 0x040000u,0x048000u,0x050000u,0x058000u,0x060000u,0x068000u,0x070000u,0x078000u,
 0x080000u,0x088000u,0x090000u,0x098000u,0x0a0000u,0x0a8000u,0x0b0000u,0x0b8000u,
 0x0c0000u,0x0c8000u,0x0d0000u,0x0d8000u,0x0e0000u,0x0e8000u,0x0f0000u,0x0f8000u,
 0x100000u,0x108000u,0x110000u,0x118000u,0x120000u,0x128000u,0x130000u,0x138000u,
 0x140000u,0x148000u,0x150000u,0x158000u,0x160000u,0x168000u,0x170000u,0x178000u,
 0x180000u,0x188000u,0x190000u,0x198000u,0x1a0000u,0x1a8000u,0x1b0000u,0x1b8000u,
 0x1c0000u,0x1c8000u,0x1d0000u,0x1d8000u,0x1e0000u,0x1e8000u,0x1f0000u,0x1f8000u,
 0x200000u,0x200000u,0x200000u,0x200000u,0x200000u,0x200000u,0x200000u,0x200000u,
 0x200000u,0x200000u,0x200000u,0x200000u,0x200000u,0x200000u,0x200000u,0x200000u,
 0x200000u,0x200000u,0x200000u,0x200000u,0x200000u,0x200000u,0x200000u,0x200000u,
 0x200000u,0x200000u,0x200000u,0x200000u,0x200000u,0x200000u,0x200000u,0x200000u,
 0x200000u,0x200000u,0x200000u,0x200000u,0x200000u,0x200000u,0x200000u,0x200000u,
 0x200000u,0x200000u,0x200000u,0x200000u,0x200000u,0x200000u,0x200000u,0x200000u,
 0x200000u,0x200000u,0x200000u,0x200000u,0x200000u,0x200000u,0x200000u,0x200000u,
 0x200000u,0x200000u,0x200000u,0x200000u,0x200000u,0x200000u,0x200000u,0x200000u,
};
static const uint32_t trx_bd_control_word_11[128]={
 0x5f29c2u,0x5f29c2u,0x5f29c2u,0x5f29c2u,0x5f29c2u,0x5f29c2u,0x5f29c2u,0x5f29c2u,
 0x5f29c2u,0x5f29c2u,0x5f29c2u,0x5f29c2u,0x5f29c2u,0x5f29c2u,0x5f29c2u,0x5f29c2u,
 0x5f29c2u,0x5f29c2u,0x5f29c2u,0x5f29c2u,0x5f29c2u,0x5f29c2u,0x5f29c2u,0x5f29c2u,
 0x5f29c2u,0x5f29c2u,0x5f29c2u,0x5f29c2u,0x5f29c2u,0x5f29c2u,0x5f29c2u,0x5f29c2u,
 0x5f29c2u,0x5f29c2u,0x5f29c2u,0x5f29c2u,0x5f29c2u,0x5f29c2u,0x5f29c2u,0x5f29c2u,
 0x5f29c2u,0x5f29c2u,0x5f29c2u,0x5f29c2u,0x5f29c2u,0x5f29c2u,0x5f29c2u,0x5f29c2u,
 0x5f29c2u,0x5f29c2u,0x5f29c2u,0x5f29c2u,0x5f29c2u,0x5f29c2u,0x5f29c2u,0x5f29c2u,
 0x5f29c2u,0x5f29c2u,0x5f29c2u,0x5f29c2u,0x5f29c2u,0x5f29c2u,0x5f29c2u,0x5f29c2u,
 0x5f29c2u,0x5fd1f3u,0x60775cu,0x611a02u,0x62571bu,0x62f199u,0x63896cu,0x641e9au,
 0x654120u,0x65ce87u,0x665964u,0x66e1bfu,0x67eb0cu,0x686c0du,0x68eaabu,0x6966ecu,
 0x6a587au,0x6acdd6u,0x6b40f7u,0x6bb1e3u,0x6c8d3eu,0x6cf7beu,0x6d602au,0x6dc68au,
 0x6e8d46u,0x6eedb3u,0x6f4c34u,0x6fa8d1u,0x705c80u,0x70b3a1u,0x7108fdu,0x715c9du,
 0x71fec5u,0x724d5cu,0x729a55u,0x72e5b6u,0x7377cfu,0x73be96u,0x7403e2u,0x7447bau,
 0x74cb2bu,0x750ad1u,0x75491eu,0x75861au,0x75fc33u,0x76355eu,0x766d50u,0x76a410u,
 0x770e0eu,0x774158u,0x777387u,0x77a4a0u,0x7803a7u,0x78319fu,0x785e98u,0x788a96u,
 0x78dfb4u,0x7908dfu,0x793123u,0x795884u,0x79a4b3u,0x79c98au,0x79ed90u,0x7a10cau,
};
static const uint32_t trx_bd_control_word_12[128]={
 0x000000u,0x010000u,0x020000u,0x030000u,0x040000u,0x050000u,0x060000u,0x070000u,
 0x080000u,0x090000u,0x0a0000u,0x0b0000u,0x0c0000u,0x0d0000u,0x0e0000u,0x0f0000u,
 0x100000u,0x110000u,0x120000u,0x130000u,0x140000u,0x150000u,0x160000u,0x170000u,
 0x180000u,0x190000u,0x1a0000u,0x1b0000u,0x1c0000u,0x1d0000u,0x1e0000u,0x1f0000u,
 0x200000u,0x210000u,0x220000u,0x230000u,0x240000u,0x250000u,0x260000u,0x270000u,
 0x280000u,0x290000u,0x2a0000u,0x2b0000u,0x2c0000u,0x2d0000u,0x2e0000u,0x2f0000u,
 0x300000u,0x310000u,0x320000u,0x330000u,0x340000u,0x350000u,0x360000u,0x370000u,
 0x380000u,0x390000u,0x3a0000u,0x3b0000u,0x3c0000u,0x3d0000u,0x3e0000u,0x3f0000u,
 0x400000u,0x410000u,0x420000u,0x430000u,0x440000u,0x450000u,0x460000u,0x470000u,
 0x480000u,0x490000u,0x4a0000u,0x4b0000u,0x4c0000u,0x4d0000u,0x4e0000u,0x4f0000u,
 0x500000u,0x510000u,0x520000u,0x530000u,0x540000u,0x550000u,0x560000u,0x570000u,
 0x580000u,0x590000u,0x5a0000u,0x5b0000u,0x5c0000u,0x5d0000u,0x5e0000u,0x5f0000u,
 0x600000u,0x610000u,0x620000u,0x630000u,0x640000u,0x650000u,0x660000u,0x670000u,
 0x680000u,0x690000u,0x6a0000u,0x6b0000u,0x6c0000u,0x6d0000u,0x6e0000u,0x6f0000u,
 0x700000u,0x710000u,0x720000u,0x730000u,0x740000u,0x750000u,0x760000u,0x770000u,
 0x780000u,0x790000u,0x7a0000u,0x7b0000u,0x7c0000u,0x7d0000u,0x7e0000u,0x7f0000u,
};
static void trx_bd_trx_bd_packet(const float k[8],uint32_t p[13]){p[0]=trx_bd_control_word_0[(unsigned)k[1]];p[1]=trx_bd_control_word_1[(unsigned)k[4]];p[2]=trx_bd_control_word_2[(unsigned)k[0]];p[3]=trx_bd_control_word_3[(unsigned)k[2]];p[4]=trx_bd_control_word_4[(unsigned)k[3]];p[5]=trx_bd_control_word_5[(unsigned)k[7]];p[6]=trx_bd_control_word_6[(unsigned)k[7]];p[7]=trx_bd_control_word_7[(unsigned)k[7]];p[8]=trx_bd_control_word_8[(unsigned)k[7]];p[9]=trx_bd_control_word_9[(unsigned)k[7]];p[10]=trx_bd_control_word_10[(unsigned)k[5]];p[11]=trx_bd_control_word_11[(unsigned)k[5]];p[12]=trx_bd_control_word_12[(unsigned)k[6]];}



#ifndef TRX_COMPACT_MODEL_MATH
#define TRX_COMPACT_MODEL_MATH


static inline long long trx_model_round(double x) {return (long long)(x+(x<0?-0.5:0.5));}
static inline uint32_t trx_bd_trx_model_word(double v) {
    if (v > 8388607.0) v = 8388607.0;
    if (v < -8388608.0) v = -8388608.0;
    return (uint32_t)(int32_t)trx_model_round(v) & 0xffffffu;
}
static inline double trx_bd_trx_model_cheb(const double *c,unsigned n,double x) {
    double b1=0,b2=0;
    for(unsigned k=n;k>1;--k) { double b=2*x*b1-b2+c[k-1]; b2=b1;b1=b; }
    return x*b1-b2+c[0];
}
#endif
static const double trx_bd_trx_bd_board_a_fit[84]={3475502.32001,3881689.93748,-347167.380822,-565496.129343,383543.856802,-27515.9457188,-163595.021913,7355274.61883,250266.807307,-334832.99695,-99308.0343962,-38740.3835181,6129.81559443,19220.7034202,5342045.23125,-1691171.27482,-16313.1794956,-1314.08621694,737.901661949,-653.44850753,298.141081529,1666778.94323,-1824915.19705,-9745.09665091,2190.9589407,105.210378721,5.6810757256,-289.943508708,-2109278.09629,-1759789.64228,27186.7929234,3293.41822787,-125.389803264,-197.839256926,-146.902848336,-5412349.396,-1371024.02253,64968.6657094,2639.64487246,-197.258921078,77.766167419,209.504543775,-7613126.74529,-714906.117011,90191.7488327,1317.94576752,23.4455391452,21.4274766358,-6.07044012732,-8282301.21166,80296.8445635,97418.7355129,-108.293927505,108.799850592,-21.0585051463,-73.6429562449,-7292373.12533,857512.492796,86064.6125898,-1805.78156867,-115.557810082,-88.108221222,-112.873231877,-4833465.10765,1477482.03131,62232.461309,-1908.12463595,-48.3202713703,-126.915340213,-47.2295564205,-2046873.00105,906181.574747,-158150.863737,35951.3888891,-3894.4347669,-1950.33445792,1802.68637158,-722037.254554,508918.334268,26158.0510431,32040.7720154,26575.355914,21269.3669261,17449.3967625};
static inline uint32_t trx_bd_trx_model_bd_board_a(unsigned i) {
    if(i>=288) return 0;
    unsigned part=i/24;
    double x=2.0*(i%24)/23.0-1.0;
    return trx_bd_trx_model_word(trx_bd_trx_model_cheb(trx_bd_trx_bd_board_a_fit+part*7,7,x));
}


#ifndef TRX_COMPACT_MODEL_MATH
#define TRX_COMPACT_MODEL_MATH


static inline long long trx_model_round(double x) {return (long long)(x+(x<0?-0.5:0.5));}


#endif
static const double trx_bd_trx_bd_board_b_fit[84]={265960.152483,325223.347541,51291.1620443,-42683.2534345,-15582.3887217,17915.573444,-22048.235113,688591.054912,198265.611937,87695.5302605,30583.9404501,-19952.7386825,-17547.2259506,-10140.794159,1093606.72399,190686.65592,14365.0263276,-15151.0111446,27172.5341946,46712.1812197,3942.54130165,4990900.27414,3497500.29418,-19433.6610322,-406482.775896,-367569.18185,-1138.651442,229614.046157,7430805.08511,-586039.366758,-110337.417765,-37810.9178598,-25828.4819852,-5246.23739861,7798.712904,4584400.65765,-2020259.80612,-8209.80328324,4646.43985043,-49919.7252477,43507.848321,-18566.6506766,-433391.881336,-2976638.11553,-36979.043758,87663.3799956,17514.8124894,9978.70370259,-15331.1736903,-5687308.06805,-1964264.09469,150215.338342,-21587.3850111,10612.771541,2190.58915796,7886.54726035,-8069905.93298,-240460.645745,300017.871899,-42413.0720492,-29619.9321134,10234.2327371,17506.1082833,-7082923.46331,965493.204063,4115.28319551,-4953.94430964,-6145.31204966,-14541.3563047,-10700.1759778,-4780160.24912,1346452.28246,-29548.8792646,-26528.4797431,31581.3228704,752.311162455,23359.0806354,-2004651.39725,1721211.50893,252152.841619,-41497.7052493,-34765.9000891,14295.7623701,35573.7750911};
static inline uint32_t trx_bd_trx_model_bd_board_b(unsigned i) {
    if(i>=288) return 0;
    unsigned part=i/24;
    double x=2.0*(i%24)/23.0-1.0;
    return trx_bd_trx_model_word(trx_bd_trx_model_cheb(trx_bd_trx_bd_board_b_fit+part*7,7,x));
}

static uint32_t trx_bd_trx_bd_board_a(unsigned i){return trx_bd_trx_model_bd_board_a(i);}
static uint32_t trx_bd_trx_bd_board_b(unsigned i){return trx_bd_trx_model_bd_board_b(i);}






static uint32_t trx_bd_bd_sine(uint16_t index){return trx_bd_local_sine(index&0x7fff);}


static inline uint32_t trx_bd_ring32(uint32_t r)   { return (r & ~0x1fu) | ((r + 1u) & 0x1fu); }
static inline uint32_t trx_bd_ring32m(uint32_t r)  { return (r & ~0x1fu) | ((r + 0x1fu) & 0x1fu); }


static void trx_bd_md_trx_bd_init(void *state)
{
    mdkit_trx_bd *s = (mdkit_trx_bd *)state;
    trx_bd_md_trx_events_init(&s->clock,&s->events);
    s->pos = 0;
    s->dec = s->strt = s->ptch = s->ramp = s->rdec = 0;
    for (unsigned i = 0; i < 5; ++i) s->clip[i] = 0;
    s->nois = s->harm = s->slew = 0;
    s->xfade = s->exc = s->decay_acc = 0;
    s->env = 0; s->clip_th = 0;
    s->fold_lo = s->fold_hi = 0;
    s->phase_a = s->phase_b = 0;
    s->tail_amp = 0; s->board_ofs = 0;
    s->fold_ref = s->fold_ref2 = 0;

    s->kv_a = 0; s->kv_b = 1; s->kv_c = 0;
    s->ph2_a = s->ph2_b = s->ph2_step = 0;
    for (unsigned i = 0; i < 0x60; ++i) { s->y[i] = 0; s->x[i] = 0; }
    for (unsigned i = 0; i < 0x20; ++i) s->out[i] = 0;
    s->r7 = 0x100;
    s->have_block = 0;
}


static void trx_bd_bd_build_controls(mdkit_trx_bd *s)
{
    uint32_t p[13];trx_bd_trx_bd_packet(s->clock.current,p);
    s->dec=p[0];s->strt=p[1];s->ptch=p[2];s->ramp=p[3];s->rdec=p[4];
    for(unsigned i=0;i<5;++i)s->clip[i]=p[5+i];
    s->nois=p[10];s->spread=p[11];s->harm=p[12];
}

static void trx_bd_md_trx_bd_set_control(void *state, unsigned i, unsigned v)
{
    trx_bd_md_trx_events_target(&((mdkit_trx_bd *)state)->events,8,i,v);
}


static void trx_bd_bd_apply_trigger(void *state)
{
    mdkit_trx_bd *s = (mdkit_trx_bd *)state;
    trx_bd_bd_build_controls(s);
    s->decay_acc = trx_bd_md_acc_to_word(trx_bd_md_add(trx_bd_md_from24(0x50c335),trx_bd_md_from24(s->decay_acc)));
    s->env       = 10;
    s->clip_th   = MDKIT_TRX_BD_TRIG_CLIP_THRESH;
    s->phase_a   = 0;
    s->phase_b   = 0;
    s->board_ofs = 0;
    s->fold_ref  = s->clip[0];
    s->fold_ref2 = s->clip[1];
    s->fold_lo   = s->clip[0];
    s->fold_hi   = s->clip[1];
    s->kv_c      = s->nois;
    s->ph2_step  = s->harm;
    s->ph2_a     = 0;
    s->ph2_b     = 0;
    s->xfade     = MDKIT_TRX_BD_TRIG_CLIP_THRESH;
    s->exc       = MDKIT_TRX_BD_TRIG_NOIS_STEP;
    s->slew      = MDKIT_TRX_BD_TRIG_SLEW;
    s->have_block = 0;
}
static void trx_bd_md_trx_bd_trigger(void *state){((mdkit_trx_bd*)state)->events.trigger=1;}

static void trx_bd_bd_run_block(mdkit_trx_bd *s, uint32_t output[32])
{
    uint32_t *y = s->y, *x = s->x, *out = s->out;
    uint64_t a = 0, b = 0;
    uint32_t x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    uint32_t r1 = 0x20, r2 = 0, r3 = 0x40, r4 = 0x20, r5 = 0, r7 = 0x100;
#define YR(i) y[(i) & 0x7fu]
#define XR(i) x[(i) & 0x7fu]
#define YO(i) out[(i) & 0x1fu]


    b = trx_bd_md_from24(s->kv_a);
    x1 = s->kv_b;
    for (int k = 0; k < 10; ++k) {
        b = trx_bd_md_add(b, trx_bd_md_from24(x1));
        x0 = x1;
        YR(r5) = trx_bd_md_mid(b); r5 = trx_bd_ring32(r5);
        YR(r5) = trx_bd_md_mid(b); r5 = trx_bd_ring32(r5);
        YR(r5) = trx_bd_md_mid(b); r5 = trx_bd_ring32(r5);
        x1 = trx_bd_md_mid(b);
        b = trx_bd_md_setmid(b, x0);
    }
    b = trx_bd_md_add(b, trx_bd_md_from24(x1));
    s->kv_a = x1;
    s->kv_b = trx_bd_md_mid(b);
    YR(r5) = trx_bd_md_mid(b); r5 = trx_bd_ring32(r5);
    YR(r5) = trx_bd_md_mid(b); r5 = trx_bd_ring32(r5);


    x0 = s->kv_c;
    x1 = s->spread;
    for (int k = 0; k < 16; ++k) {
        y0 = YR(r5); r5 = trx_bd_ring32(r5);
        y1 = YR(r5); r5 = trx_bd_ring32m(r5);
        a = trx_bd_md_mpy(y0, x0);
        b = trx_bd_md_mpy(x0, y1);
        YR(r5) = trx_bd_md_acc_to_word(a); r5 = trx_bd_ring32(r5);
        YR(r5) = trx_bd_md_acc_to_word(b); r5 = trx_bd_ring32(r5);
    }
    a = trx_bd_md_mpy(x1, x0);
    s->kv_c = trx_bd_md_acc_to_word(a);


    {
        uint32_t env = s->env;
        if (env == 0) goto normal;
        env -= 1;
        s->env = env;
        if (env == 9) goto tail;
        goto alt;
    }

tail:

    x0 = 0x733333u;
    x1 = s->tail_amp;
    for (int k = 0; k < 32; ++k) {
        a = trx_bd_md_mpy(x1, x0);
        YO(r7) = trx_bd_md_acc_to_word(a); output[k] = trx_bd_md_acc_to_word(a); r7 = trx_bd_ring32(r7);
        x1 = trx_bd_md_mid(a);
    }
    goto done;

alt:

    {
        const uint32_t xoff = s->board_ofs;
        x0 = s->xfade;
        x1 = s->strt;
        for (int k = 0; k < 32; ++k) {
            y1 = trx_bd_trx_bd_board_b(xoff + k);
            y0 = trx_bd_trx_bd_board_a(xoff + k);
            a = trx_bd_md_mpy(y1, x1);
            y1 = YR(r5); r5 = trx_bd_ring32(r5);
            a = trx_bd_md_asl(a, 1);
            a = trx_bd_md_asl(a, 1);
            a = trx_bd_md_mac(a, y0, x0);
            a = trx_bd_md_add(a, trx_bd_md_from24(y1));
            YO(r7) = trx_bd_md_acc_to_word(a); output[k] = trx_bd_md_acc_to_word(a); r7 = trx_bd_ring32(r7);
        }
        s->board_ofs = 0x20 + xoff;
    }
    goto done;

normal:

    a = trx_bd_md_asr(trx_bd_md_from24(s->ptch), 6);
    y0 = s->ramp;
    y1 = s->clip_th;
    b = trx_bd_md_asr(trx_bd_md_mpy(y1, y0), 1);
    a = trx_bd_md_add(a, b);
    b = trx_bd_md_from48(trx_bd_md_pack48(s->phase_a, s->phase_b));
    x1 = trx_bd_md_mid(a); x0 = trx_bd_md_low(a);
    for (int k = 0; k < 32; ++k) {
        b = trx_bd_md_and_mid(b, 0x7fffu);
        r2 = (uint16_t)trx_bd_md_mid(b);
        b = trx_bd_md_add(b, trx_bd_md_from48(trx_bd_md_pack48(x1, x0)));
        a = trx_bd_md_from24(trx_bd_bd_sine(r2));
        a = trx_bd_md_asr(a, 2);
        XR(r4) = trx_bd_md_acc_to_word(a); r4 = trx_bd_ring32(r4);
    }
    s->phase_a = trx_bd_md_mid(b);
    s->phase_b = trx_bd_md_low(b);

    a = trx_bd_md_asr(trx_bd_md_from24(s->ptch), 7);
    y0 = s->ramp;
    y1 = s->clip_th;
    b = trx_bd_md_asr(trx_bd_md_mpy(y1, y0), 1);
    a = trx_bd_md_add(a, b);
    y0 = s->rdec;
    s->clip_th = trx_bd_md_mid(trx_bd_md_mpy(y1, y0));

    r1 = 0x20;
    b = trx_bd_md_from48(trx_bd_md_pack48(s->ph2_a, s->ph2_b));
    x1 = trx_bd_md_mid(a); x0 = trx_bd_md_low(a);
    for (int k = 0; k < 32; ++k) {
        b = trx_bd_md_and_mid(b, 0x7fffu);
        r2 = (uint16_t)trx_bd_md_mid(b);
        b = trx_bd_md_add(b, trx_bd_md_from48(trx_bd_md_pack48(x1, x0)));
        a = trx_bd_md_asl(trx_bd_md_from24(trx_bd_bd_sine(r2)), 1);
        YR(r1) = trx_bd_md_acc_to_word(a); r1 = trx_bd_ring32(r1);
    }
    s->ph2_a = trx_bd_md_mid(b);
    s->ph2_b = trx_bd_md_low(b);

    x0 = s->slew;
    x1 = s->ph2_step;
    for (int k = 0; k < 32; ++k) {
        b = trx_bd_md_from24(XR(r4));
        y1 = YR(r1); r1 = trx_bd_ring32(r1);
        a = trx_bd_md_asr(trx_bd_md_mpy(y1, x1), 4);
        b = trx_bd_md_add(b, a);
        XR(r4) = trx_bd_md_acc_to_word(b); r4 = trx_bd_ring32(r4);
    }
    a = trx_bd_md_mpy(x1, x0);
    s->ph2_step = trx_bd_md_acc_to_word(a);


    x0 = s->fold_ref;
    x1 = s->fold_ref2;
    a = trx_bd_md_add(trx_bd_md_from24(s->exc), trx_bd_md_from24(x0));
    s->fold_ref = trx_bd_md_acc_to_word(a);
    a = trx_bd_md_sub(0, trx_bd_md_from24(s->exc));
    a = trx_bd_md_add(a, trx_bd_md_from24(x1));
    s->fold_ref2 = trx_bd_md_acc_to_word(a);
    x[0x06] = x0;
    x[0x07] = x1;

    y0 = s->fold_lo;
    y1 = s->fold_hi;
    a = trx_bd_md_from24(s->exc);
    for (int k = 0; k < 32; ++k) {
        b = trx_bd_md_asr(trx_bd_md_sub(0, trx_bd_md_from24(XR(r4))), 1);
        if (trx_bd_md_s56(b) > trx_bd_md_s56(trx_bd_md_from24(x0))) {
            b = trx_bd_md_sub(trx_bd_md_from24(y0), a);
            y1 = x[0x07];
            y0 = trx_bd_md_mid(b);
        } else if (trx_bd_md_s56(b) < trx_bd_md_s56(trx_bd_md_from24(x1))) {
            b = trx_bd_md_add(trx_bd_md_from24(y1), a);
            y0 = x[0x06];
            y1 = trx_bd_md_mid(b);
        }
        b = trx_bd_md_asl(b, 1);
        XR(r4) = trx_bd_md_acc_to_word(b); r4 = trx_bd_ring32(r4);
    }
    s->fold_lo = y0;
    s->fold_hi = y1;

    x0 = s->dec;
    x1 = s->decay_acc;
    for (int k = 0; k < 32; ++k) {
        y0 = XR(r4);
        y1 = YR(r5); r5 = trx_bd_ring32(r5);
        a = trx_bd_md_add(trx_bd_md_mpy(x1, y0), trx_bd_md_mpy(y1, x1));
        b = trx_bd_md_mpy(x1, x0);
        XR(r4) = trx_bd_md_acc_to_word(a); r4 = trx_bd_ring32(r4);
        x1 = trx_bd_md_mid(b);
    }
    s->decay_acc = x1;

    y1 = s->clip[4];
    x0 = s->clip[2];
    x1 = s->clip[3];
    for (int k = 0; k < 32; ++k) {
        a = trx_bd_md_from24(XR(r4)); r4 = trx_bd_ring32(r4);
        if (trx_bd_md_s56(a) >= trx_bd_md_s56(trx_bd_md_from24(x0))) {
            a = trx_bd_md_sub(a, trx_bd_md_from24(x0));
            a = trx_bd_md_add(trx_bd_md_mpy(trx_bd_md_mid(a), x0), trx_bd_md_from24(x0));
        } else if (trx_bd_md_s56(a) <= trx_bd_md_s56(trx_bd_md_from24(x1))) {
            a = trx_bd_md_sub(a, trx_bd_md_from24(x1));
            a = trx_bd_md_add(trx_bd_md_mpy(trx_bd_md_mid(a), x0), trx_bd_md_from24(x1));
        }
        a = trx_bd_md_mac(a, y1, trx_bd_md_mid(a));
        XR(r3) = trx_bd_md_acc_to_word(a); r3 = trx_bd_ring32(r3);
    }

    for (int k = 0; k < 16; ++k) {
        a = trx_bd_md_from24(XR(r3)); r3 = trx_bd_ring32(r3);
        b = trx_bd_md_from24(XR(r3)); r3 = trx_bd_ring32(r3);
        x1 = XR(r4); r4 = trx_bd_ring32(r4);
        y1 = XR(r4); r4 = trx_bd_ring32(r4);
        a = trx_bd_md_asl(trx_bd_md_add(a, trx_bd_md_from24(x1)), 1);
        b = trx_bd_md_asl(trx_bd_md_add(b, trx_bd_md_from24(y1)), 1);
        YO(r7) = trx_bd_md_acc_to_word(a); output[2*k+0] = trx_bd_md_acc_to_word(a); r7 = trx_bd_ring32(r7);
        YO(r7) = trx_bd_md_acc_to_word(b); output[2*k+1] = trx_bd_md_acc_to_word(b); r7 = trx_bd_ring32(r7);
    }
    r7 = trx_bd_ring32m(r7);
    s->tail_amp = YO(r7);

done:
    s->r7 = r7;
#undef YR
#undef XR
#undef YO
}

static void trx_bd_md_trx_bd_render(void *state, float *out, unsigned frames)
{
    mdkit_trx_bd *s = (mdkit_trx_bd *)state;
    while (frames) {
        if (s->pos == 0u) {
            unsigned event=trx_bd_md_trx_events_boundary(&s->clock,&s->events,0);

            if((event&trx_bd_MD_TRX_PUBLISH) || s->clock.block==1)trx_bd_bd_build_controls(s);
            if(event&trx_bd_MD_TRX_FIRST){s->kv_a=0;s->kv_b=1;}
            if(event&trx_bd_MD_TRX_TRIGGER)trx_bd_bd_apply_trigger(s);
            s->have_block = 0;
        }
        if (!s->have_block) {
            trx_bd_bd_run_block(s, s->block);
            s->have_block = 1;
        }
        unsigned n = trx_bd_MD_VOICE_BLOCK - s->pos;
        if (n > frames) n = frames;
        for (unsigned i = 0; i < n; ++i) {
            const uint32_t w = s->block[s->pos + i] & trx_bd_MD_MASK24;
            out[i] = (float)(int32_t)((w & 0x800000u) ? (int32_t)w - 0x1000000 : (int32_t)w) / 8388608.0f;
        }
        out += n; frames -= n; s->pos += n;
        if (s->pos >= trx_bd_MD_VOICE_BLOCK) s->pos = 0;
    }
}

static const char *const trx_bd_md_trx_bd_control_names[MDKIT_TRX_BD_CONTROLS] = {
    "PTCH", "DEC", "RAMP", "RDEC", "STRT", "NOIS", "HARM", "CLIP"
};

const mdkit_voice mdkit_voice_trx_bd = {
    "TRX-BD", 16u, MDKIT_TRX_BD_CONTROLS, trx_bd_md_trx_bd_control_names,
    sizeof(mdkit_trx_bd),
    trx_bd_md_trx_bd_init, trx_bd_md_trx_bd_set_control, trx_bd_md_trx_bd_trigger, trx_bd_md_trx_bd_render, mdkit_trx_bd_defaults
};
