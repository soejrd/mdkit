/* SPDX-License-Identifier: CC0-1.0
 * TRX-CB standalone synthesis voice.
 */
#include "mdkit_trx_cb.h"

/* ---- Default controls --------------------------------------------------- */
/* Active controls are in descriptor order; unused clock slots stay at 64. */
static const unsigned char mdkit_trx_cb_defaults[MDKIT_MAX_CONTROLS] = {
    100, 50, 127, 127, 0, 0, 64, 64
};
#define trx_cb_MD_MASK24 UINT32_C(0xffffff)
#define trx_cb_MD_MASK48 UINT64_C(0xffffffffffff)
#define trx_cb_MD_MASK56 UINT64_C(0xffffffffffffff)
#define trx_cb_MD_VOICE_BLOCK 32u
enum { trx_cb_MD_TRX_PUBLISH=1,trx_cb_MD_TRX_TRIGGER=2,trx_cb_MD_TRX_FIRST=4 };
static inline int64_t trx_cb_md_signed_bits(uint64_t word, unsigned bits)
{
    const uint64_t sign = UINT64_C(1) << (bits - 1);
    const uint64_t mask = (UINT64_C(1) << bits) - 1;
    word &= mask;
    return (word & sign) ? (int64_t)word - (int64_t)(UINT64_C(1) << bits)
                         : (int64_t)word;
}
static inline int64_t trx_cb_md_s24(uint32_t w) { return trx_cb_md_signed_bits(w & trx_cb_MD_MASK24, 24); }
static inline int64_t trx_cb_md_s56(uint64_t a) { return trx_cb_md_signed_bits(a & trx_cb_MD_MASK56, 56); }
static inline uint32_t trx_cb_md_mid(uint64_t a) { return (uint32_t)((a >> 24) & trx_cb_MD_MASK24); }
static inline uint32_t trx_cb_md_low(uint64_t a) { return (uint32_t)(a & trx_cb_MD_MASK24); }
static inline uint64_t trx_cb_md_from24(uint32_t w)
{ return ((uint64_t)(trx_cb_md_s24(w) * (INT64_C(1) << 24))) & trx_cb_MD_MASK56; }
static inline uint64_t trx_cb_md_from48(uint64_t w)
{ return (uint64_t)trx_cb_md_signed_bits(w & trx_cb_MD_MASK48, 48) & trx_cb_MD_MASK56; }
static inline uint64_t trx_cb_md_setmid(uint64_t a, uint32_t w)
{ return (a & ~(UINT64_C(0xffffff) << 24)) | ((uint64_t)(w & trx_cb_MD_MASK24) << 24); }
static inline uint64_t trx_cb_md_add(uint64_t a, uint64_t b) { return (a + b) & trx_cb_MD_MASK56; }
static inline uint64_t trx_cb_md_sub(uint64_t a, uint64_t b) { return (a - b) & trx_cb_MD_MASK56; }
static inline uint64_t trx_cb_md_asl(uint64_t a, unsigned n) { return (a << n) & trx_cb_MD_MASK56; }
static inline uint64_t trx_cb_md_asr(uint64_t a, unsigned n)
{
    const int64_t v = trx_cb_md_s56(a);
    const int64_t sh = v >= 0 ? v / (INT64_C(1) << n)
        : -((-v + (INT64_C(1) << n) - 1) / (INT64_C(1) << n));
    return (uint64_t)sh & trx_cb_MD_MASK56;
}
static inline uint64_t trx_cb_md_mpy(uint32_t a, uint32_t b)
{ return (uint64_t)(trx_cb_md_s24(a) * trx_cb_md_s24(b) * 2) & trx_cb_MD_MASK56; }
static inline uint64_t trx_cb_md_mac(uint64_t acc, uint32_t x, uint32_t y)
{
    return (acc + trx_cb_md_mpy(x, y)) & trx_cb_MD_MASK56;
}
static inline uint64_t trx_cb_md_and_mid(uint64_t a, uint32_t mask)
{ return trx_cb_md_setmid(a, trx_cb_md_mid(a) & mask); }
static inline uint64_t trx_cb_md_pack48(uint32_t hi, uint32_t lo)
{ return ((uint64_t)(hi & trx_cb_MD_MASK24) << 24) | (lo & trx_cb_MD_MASK24); }
static inline uint32_t trx_cb_md_acc_to_word(uint64_t a)
{
    const int64_t v = trx_cb_md_s56(a);
    if(v < -(INT64_C(0x800000) << 24)) return UINT32_C(0x800000);
    if(v >  (INT64_C(0x7fffff) << 24)) return UINT32_C(0x7fffff);
    return trx_cb_md_mid(a);
}
static inline void trx_cb_md_control_clock_seed(mdkit_control_clock *c, unsigned count,
                                         const unsigned char *knobs)
{
    c->count = count;
    for (unsigned i = 0; i < count; ++i) {
        const float v = (float)knobs[i];
        c->target[i] = c->raw[i] = c->current[i] = c->pending[i] = v;
    }
    c->block = 0; c->next_update = 0; c->publish_block = 0; c->pending_valid = 0;
}
static inline void trx_cb_md_trx_events_init(mdkit_control_clock *c,mdkit_trx_cb_events *e)
{

    trx_cb_md_control_clock_seed(c,8,mdkit_trx_cb_defaults); c->next_update=2;
    e->targets=e->trigger=e->triggered=0;
    for(unsigned i=0;i<8;++i){e->queued[i]=mdkit_trx_cb_defaults[i];e->last_raw[i]=(unsigned)mdkit_trx_cb_defaults[i] * 128u;}
}
static inline void trx_cb_md_trx_events_target(mdkit_trx_cb_events *e,unsigned count,unsigned i,unsigned value)
{ if(i<count){e->queued[i]=value>127?127:value;e->targets|=1u<<i;} }
static inline unsigned trx_cb_md_trx_events_boundary(mdkit_control_clock *c,mdkit_trx_cb_events *e,int trigger_only)
{
    unsigned result=0;
    if(c->pending_valid && c->block>=c->publish_block){
        for(unsigned i=0;i<8;++i)c->current[i]=c->pending[i];
        c->pending_valid=0;result=trx_cb_MD_TRX_PUBLISH;
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
        result=trx_cb_MD_TRX_PUBLISH|trx_cb_MD_TRX_TRIGGER|(e->triggered?0:trx_cb_MD_TRX_FIRST);
        e->trigger=0;e->triggered=1;
    }
    ++c->block;return result;
}
static uint32_t trx_cb_local_sine(unsigned index) {
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

static const uint32_t trx_cb_control_word_0[128]={
 0x4b7dc8u,0x4cbae9u,0x4df33bu,0x505522u,0x517e97u,0x53c245u,0x54dc6cu,0x55f16cu,
 0x580be8u,0x591163u,0x5b0cddu,0x5c02e5u,0x5cf3d2u,0x5ec67au,0x5fa849u,0x615d16u,
 0x62302du,0x62fe77u,0x648cdfu,0x654d1cu,0x66bffdu,0x6772c3u,0x68212fu,0x697143u,
 0x6a130fu,0x6b4a8au,0x6be05fu,0x6c725eu,0x6d8b23u,0x6e1211u,0x6f1560u,0x6f91e7u,
 0x708115u,0x70f3e0u,0x716390u,0x7239ecu,0x72a0bau,0x7365eeu,0x73c477u,0x74205fu,
 0x74d091u,0x7524fbu,0x75c6beu,0x761436u,0x765f7bu,0x76efa6u,0x7734a9u,0x77b8cdu,
 0x77f808u,0x783570u,0x78aaf9u,0x78e332u,0x794ec8u,0x79823au,0x79b42cu,0x7a13b6u,
 0x7a4163u,0x7a98bfu,0x7ac280u,0x7aeb05u,0x7b3880u,0x7b5d86u,0x7ba44eu,0x7bc61fu,
 0x7c06c3u,0x7c25a5u,0x7c439au,0x7c7cdau,0x7c9832u,0x7ccc73u,0x7ce567u,0x7cfd9bu,
 0x7d2bdau,0x7d41efu,0x7d6c1eu,0x7d8042u,0x7d93cau,0x7db91au,0x7dcaeau,0x7decf0u,
 0x7dfd2du,0x7e0cedu,0x7e2b01u,0x7e395cu,0x7e54c6u,0x7e61dcu,0x7e6e8cu,0x7e86c7u,
 0x7e9257u,0x7ea86bu,0x7eb2f5u,0x7ebd2cu,0x7ed0adu,0x7ed9fcu,0x7eebc2u,0x7ef43du,
 0x7f046du,0x7f0c27u,0x7f13a5u,0x7f21f1u,0x7f28c4u,0x7f35cau,0x7f3c01u,0x7f4207u,
 0x7f4d87u,0x7f5304u,0x7f5d7du,0x7f627cu,0x7f6754u,0x7f7094u,0x7f74fdu,0x7f7d69u,
 0x7f816du,0x7f8552u,0x7f8cc1u,0x7f904du,0x7f9712u,0x7f9a4du,0x7f9d6eu,0x7fa368u,
 0x7fa641u,0x7fabb2u,0x7fae4au,0x7fb0ceu,0x7fb59cu,0x7fb7e6u,0x7fbc45u,0x7fbe5bu,
};
static const uint32_t trx_cb_control_word_1[128]={
 0x000000u,0x000000u,0x000000u,0x000000u,0x000000u,0x000000u,0x000000u,0x000000u,
 0x000000u,0x000001u,0x000001u,0x000002u,0x000002u,0x000004u,0x000006u,0x000009u,
 0x000016u,0x000020u,0x00002fu,0x000044u,0x000062u,0x00008bu,0x0000c3u,0x00010fu,
 0x0001fcu,0x0002aeu,0x000394u,0x0004beu,0x00063au,0x00081cu,0x000a7au,0x000d6du,
 0x00158eu,0x001b05u,0x0021a1u,0x002994u,0x003311u,0x003e54u,0x004b9bu,0x005b2bu,
 0x00824fu,0x009a84u,0x00b642u,0x00d5e4u,0x00f9c7u,0x01224bu,0x014fd3u,0x0182c1u,
 0x01fa5fu,0x023fd5u,0x028c3bu,0x02dfeeu,0x033b4au,0x039ea3u,0x040a4cu,0x047e90u,
 0x0581f2u,0x061187u,0x06aa9eu,0x074d5fu,0x07f9e8u,0x08b04eu,0x09709cu,0x0a3ad8u,
 0x0becf4u,0x0cd4aeu,0x0dc60au,0x0ec0deu,0x0fc4fau,0x10d227u,0x11e825u,0x1306aeu,
 0x155c2cu,0x169278u,0x17cffeu,0x19145eu,0x1a5f35u,0x1bb01bu,0x1d06a7u,0x1e626eu,
 0x2127f8u,0x2290deu,0x23fd48u,0x256cc6u,0x26deebu,0x28534bu,0x29c97cu,0x2b4113u,
 0x2e32dfu,0x2fac4cu,0x312595u,0x329e5du,0x34164au,0x358d09u,0x370246u,0x3875b3u,
 0x3b55f7u,0x3cc243u,0x3e2ba9u,0x3f91efu,0x40f4dau,0x425438u,0x43afd6u,0x450787u,
 0x47aa7cu,0x48f576u,0x4a3beeu,0x4b7dc8u,0x4cbae9u,0x4df33bu,0x4f26a9u,0x505522u,
 0x52a2fbu,0x53c245u,0x54dc6cu,0x55f16cu,0x570140u,0x580be8u,0x591163u,0x5a11b4u,
 0x5c02e5u,0x5cf3d2u,0x5ddfabu,0x5ec67au,0x5fa849u,0x608524u,0x615d16u,0x62302du,
};
static const uint32_t trx_cb_control_word_2[128]={
 0x0002c7u,0x0002c8u,0x0002c9u,0x0002cau,0x0002ccu,0x0002cdu,0x0002ceu,0x0002d0u,
 0x0002d1u,0x0002d2u,0x0002d4u,0x0002d5u,0x0002d6u,0x0002d8u,0x0002d9u,0x0002dau,
 0x0002dcu,0x0002ddu,0x0002deu,0x0002e0u,0x0002e1u,0x0002e2u,0x0002e4u,0x0002e5u,
 0x0002e6u,0x0002e7u,0x0002e9u,0x0002eau,0x0002ebu,0x0002edu,0x0002eeu,0x0002efu,
 0x0002f1u,0x0002f2u,0x0002f3u,0x0002f5u,0x0002f6u,0x0002f7u,0x0002f9u,0x0002fau,
 0x0002fbu,0x0002fdu,0x0002feu,0x0002ffu,0x000301u,0x000302u,0x000303u,0x000305u,
 0x000306u,0x000307u,0x000308u,0x00030au,0x00030bu,0x00030cu,0x00030eu,0x00030fu,
 0x000310u,0x000312u,0x000313u,0x000314u,0x000316u,0x000317u,0x000318u,0x00031au,
 0x00031bu,0x00031cu,0x00031eu,0x00031fu,0x000320u,0x000322u,0x000323u,0x000324u,
 0x000326u,0x000327u,0x000328u,0x000329u,0x00032bu,0x00032cu,0x00032du,0x00032fu,
 0x000330u,0x000331u,0x000333u,0x000334u,0x000335u,0x000337u,0x000338u,0x000339u,
 0x00033bu,0x00033cu,0x00033du,0x00033fu,0x000340u,0x000341u,0x000343u,0x000344u,
 0x000345u,0x000347u,0x000348u,0x000349u,0x00034au,0x00034cu,0x00034du,0x00034eu,
 0x000350u,0x000351u,0x000352u,0x000354u,0x000355u,0x000356u,0x000358u,0x000359u,
 0x00035au,0x00035cu,0x00035du,0x00035eu,0x000360u,0x000361u,0x000362u,0x000364u,
 0x000365u,0x000366u,0x000367u,0x000369u,0x00036au,0x00036bu,0x00036du,0x00036eu,
};
static const uint32_t trx_cb_control_word_3[128]={
 0x0003d7u,0x0003d8u,0x0003dau,0x0003dbu,0x0003ddu,0x0003deu,0x0003e0u,0x0003e1u,
 0x0003e3u,0x0003e5u,0x0003e6u,0x0003e8u,0x0003e9u,0x0003ebu,0x0003ecu,0x0003eeu,
 0x0003efu,0x0003f1u,0x0003f3u,0x0003f4u,0x0003f6u,0x0003f7u,0x0003f9u,0x0003fau,
 0x0003fcu,0x0003feu,0x0003ffu,0x000401u,0x000402u,0x000404u,0x000405u,0x000407u,
 0x000408u,0x00040au,0x00040cu,0x00040du,0x00040fu,0x000410u,0x000412u,0x000413u,
 0x000415u,0x000417u,0x000418u,0x00041au,0x00041bu,0x00041du,0x00041eu,0x000420u,
 0x000421u,0x000423u,0x000425u,0x000426u,0x000428u,0x000429u,0x00042bu,0x00042cu,
 0x00042eu,0x00042fu,0x000431u,0x000433u,0x000434u,0x000436u,0x000437u,0x000439u,
 0x00043au,0x00043cu,0x00043eu,0x00043fu,0x000441u,0x000442u,0x000444u,0x000445u,
 0x000447u,0x000448u,0x00044au,0x00044cu,0x00044du,0x00044fu,0x000450u,0x000452u,
 0x000453u,0x000455u,0x000457u,0x000458u,0x00045au,0x00045bu,0x00045du,0x00045eu,
 0x000460u,0x000461u,0x000463u,0x000465u,0x000466u,0x000468u,0x000469u,0x00046bu,
 0x00046cu,0x00046eu,0x00046fu,0x000471u,0x000473u,0x000474u,0x000476u,0x000477u,
 0x000479u,0x00047au,0x00047cu,0x00047eu,0x00047fu,0x000481u,0x000482u,0x000484u,
 0x000485u,0x000487u,0x000488u,0x00048au,0x00048cu,0x00048du,0x00048fu,0x000490u,
 0x000492u,0x000493u,0x000495u,0x000497u,0x000498u,0x00049au,0x00049bu,0x00049du,
};
static const uint32_t trx_cb_control_word_4[128]={
 0x5fffffu,0x5f7fffu,0x5effffu,0x5e7fffu,0x5dffffu,0x5d7fffu,0x5cffffu,0x5c7fffu,
 0x5bffffu,0x5b7fffu,0x5affffu,0x5a7fffu,0x59ffffu,0x597fffu,0x58ffffu,0x587fffu,
 0x57ffffu,0x577fffu,0x56ffffu,0x567fffu,0x55ffffu,0x557fffu,0x54ffffu,0x547fffu,
 0x53ffffu,0x537fffu,0x52ffffu,0x527fffu,0x51ffffu,0x517fffu,0x50ffffu,0x507fffu,
 0x4fffffu,0x4f7fffu,0x4effffu,0x4e7fffu,0x4dffffu,0x4d7fffu,0x4cffffu,0x4c7fffu,
 0x4bffffu,0x4b7fffu,0x4affffu,0x4a7fffu,0x49ffffu,0x497fffu,0x48ffffu,0x487fffu,
 0x47ffffu,0x477fffu,0x46ffffu,0x467fffu,0x45ffffu,0x457fffu,0x44ffffu,0x447fffu,
 0x43ffffu,0x437fffu,0x42ffffu,0x427fffu,0x41ffffu,0x417fffu,0x40ffffu,0x407fffu,
 0x3fffffu,0x3f7fffu,0x3effffu,0x3e7fffu,0x3dffffu,0x3d7fffu,0x3cffffu,0x3c7fffu,
 0x3bffffu,0x3b7fffu,0x3affffu,0x3a7fffu,0x39ffffu,0x397fffu,0x38ffffu,0x387fffu,
 0x37ffffu,0x377fffu,0x36ffffu,0x367fffu,0x35ffffu,0x357fffu,0x34ffffu,0x347fffu,
 0x33ffffu,0x337fffu,0x32ffffu,0x327fffu,0x31ffffu,0x317fffu,0x30ffffu,0x307fffu,
 0x2fffffu,0x2f7fffu,0x2effffu,0x2e7fffu,0x2dffffu,0x2d7fffu,0x2cffffu,0x2c7fffu,
 0x2bffffu,0x2b7fffu,0x2affffu,0x2a7fffu,0x29ffffu,0x297fffu,0x28ffffu,0x287fffu,
 0x27ffffu,0x277fffu,0x26ffffu,0x267fffu,0x25ffffu,0x257fffu,0x24ffffu,0x247fffu,
 0x23ffffu,0x237fffu,0x22ffffu,0x227fffu,0x21ffffu,0x217fffu,0x20ffffu,0x207fffu,
};
static const uint32_t trx_cb_control_word_5[128]={
 0x200000u,0x208000u,0x210000u,0x218000u,0x220000u,0x228000u,0x230000u,0x238000u,
 0x240000u,0x248000u,0x250000u,0x258000u,0x260000u,0x268000u,0x270000u,0x278000u,
 0x280000u,0x288000u,0x290000u,0x298000u,0x2a0000u,0x2a8000u,0x2b0000u,0x2b8000u,
 0x2c0000u,0x2c8000u,0x2d0000u,0x2d8000u,0x2e0000u,0x2e8000u,0x2f0000u,0x2f8000u,
 0x300000u,0x308000u,0x310000u,0x318000u,0x320000u,0x328000u,0x330000u,0x338000u,
 0x340000u,0x348000u,0x350000u,0x358000u,0x360000u,0x368000u,0x370000u,0x378000u,
 0x380000u,0x388000u,0x390000u,0x398000u,0x3a0000u,0x3a8000u,0x3b0000u,0x3b8000u,
 0x3c0000u,0x3c8000u,0x3d0000u,0x3d8000u,0x3e0000u,0x3e8000u,0x3f0000u,0x3f8000u,
 0x400000u,0x408000u,0x410000u,0x418000u,0x420000u,0x428000u,0x430000u,0x438000u,
 0x440000u,0x448000u,0x450000u,0x458000u,0x460000u,0x468000u,0x470000u,0x478000u,
 0x480000u,0x488000u,0x490000u,0x498000u,0x4a0000u,0x4a8000u,0x4b0000u,0x4b8000u,
 0x4c0000u,0x4c8000u,0x4d0000u,0x4d8000u,0x4e0000u,0x4e8000u,0x4f0000u,0x4f8000u,
 0x500000u,0x508000u,0x510000u,0x518000u,0x520000u,0x528000u,0x530000u,0x538000u,
 0x540000u,0x548000u,0x550000u,0x558000u,0x560000u,0x568000u,0x570000u,0x578000u,
 0x580000u,0x588000u,0x590000u,0x598000u,0x5a0000u,0x5a8000u,0x5b0000u,0x5b8000u,
 0x5c0000u,0x5c8000u,0x5d0000u,0x5d8000u,0x5e0000u,0x5e8000u,0x5f0000u,0x5f8000u,
};
static const uint32_t trx_cb_control_word_6[128]={
 0x1e0000u,0x1ec000u,0x1f8000u,0x204000u,0x210000u,0x21c000u,0x228000u,0x234000u,
 0x240000u,0x24c000u,0x258000u,0x264000u,0x270000u,0x27c000u,0x288000u,0x294000u,
 0x2a0000u,0x2ac000u,0x2b8000u,0x2c4000u,0x2d0000u,0x2dc000u,0x2e8000u,0x2f4000u,
 0x300000u,0x30c000u,0x318000u,0x324000u,0x330000u,0x33c000u,0x348000u,0x354000u,
 0x360000u,0x36c000u,0x378000u,0x384000u,0x390000u,0x39c000u,0x3a8000u,0x3b4000u,
 0x3c0000u,0x3cc000u,0x3d8000u,0x3e4000u,0x3f0000u,0x3fc000u,0x408000u,0x414000u,
 0x420000u,0x42c000u,0x438000u,0x444000u,0x450000u,0x45c000u,0x468000u,0x474000u,
 0x480000u,0x48c000u,0x498000u,0x4a4000u,0x4b0000u,0x4bc000u,0x4c8000u,0x4d4000u,
 0x4e0000u,0x4ec000u,0x4f8000u,0x504000u,0x510000u,0x51c000u,0x528000u,0x534000u,
 0x540000u,0x54c000u,0x558000u,0x564000u,0x570000u,0x57c000u,0x588000u,0x594000u,
 0x5a0000u,0x5ac000u,0x5b8000u,0x5c4000u,0x5d0000u,0x5dc000u,0x5e8000u,0x5f4000u,
 0x600000u,0x60c000u,0x618000u,0x624000u,0x630000u,0x63c000u,0x648000u,0x654000u,
 0x660000u,0x66c000u,0x678000u,0x684000u,0x690000u,0x69c000u,0x6a8000u,0x6b4000u,
 0x6c0000u,0x6cc000u,0x6d8000u,0x6e4000u,0x6f0000u,0x6fc000u,0x708000u,0x714000u,
 0x720000u,0x72c000u,0x738000u,0x744000u,0x750000u,0x75c000u,0x768000u,0x774000u,
 0x780000u,0x78c000u,0x798000u,0x7a4000u,0x7b0000u,0x7bc000u,0x7c8000u,0x7d4000u,
};
static const uint32_t trx_cb_control_word_7[128]={
 0x1e0000u,0x1ec000u,0x1f8000u,0x204000u,0x210000u,0x21c000u,0x228000u,0x234000u,
 0x240000u,0x24c000u,0x258000u,0x264000u,0x270000u,0x27c000u,0x288000u,0x294000u,
 0x2a0000u,0x2ac000u,0x2b8000u,0x2c4000u,0x2d0000u,0x2dc000u,0x2e8000u,0x2f4000u,
 0x300000u,0x30c000u,0x318000u,0x324000u,0x330000u,0x33c000u,0x348000u,0x354000u,
 0x360000u,0x36c000u,0x378000u,0x384000u,0x390000u,0x39c000u,0x3a8000u,0x3b4000u,
 0x3c0000u,0x3cc000u,0x3d8000u,0x3e4000u,0x3f0000u,0x3fc000u,0x408000u,0x414000u,
 0x420000u,0x42c000u,0x438000u,0x444000u,0x450000u,0x45c000u,0x468000u,0x474000u,
 0x480000u,0x48c000u,0x498000u,0x4a4000u,0x4b0000u,0x4bc000u,0x4c8000u,0x4d4000u,
 0x4e0000u,0x4ec000u,0x4f8000u,0x504000u,0x510000u,0x51c000u,0x528000u,0x534000u,
 0x540000u,0x54c000u,0x558000u,0x564000u,0x570000u,0x57c000u,0x588000u,0x594000u,
 0x5a0000u,0x5ac000u,0x5b8000u,0x5c4000u,0x5d0000u,0x5dc000u,0x5e8000u,0x5f4000u,
 0x600000u,0x60c000u,0x618000u,0x624000u,0x630000u,0x63c000u,0x648000u,0x654000u,
 0x660000u,0x66c000u,0x678000u,0x684000u,0x690000u,0x69c000u,0x6a8000u,0x6b4000u,
 0x6c0000u,0x6cc000u,0x6d8000u,0x6e4000u,0x6f0000u,0x6fc000u,0x708000u,0x714000u,
 0x720000u,0x72c000u,0x738000u,0x744000u,0x750000u,0x75c000u,0x768000u,0x774000u,
 0x780000u,0x78c000u,0x798000u,0x7a4000u,0x7b0000u,0x7bc000u,0x7c8000u,0x7d4000u,
};
static const uint32_t trx_cb_control_word_8[128]={
 0x0004e2u,0x0004e1u,0x0004e0u,0x0004dfu,0x0004deu,0x0004ddu,0x0004dcu,0x0004dbu,
 0x0004dau,0x0004d9u,0x0004d8u,0x0004d7u,0x0004d6u,0x0004d5u,0x0004d4u,0x0004d3u,
 0x0004d2u,0x0004d1u,0x0004d0u,0x0004cfu,0x0004ceu,0x0004cdu,0x0004ccu,0x0004cbu,
 0x0004cau,0x0004c9u,0x0004c8u,0x0004c7u,0x0004c6u,0x0004c5u,0x0004c4u,0x0004c3u,
 0x0004c2u,0x0004c1u,0x0004c0u,0x0004bfu,0x0004beu,0x0004bdu,0x0004bcu,0x0004bbu,
 0x0004bau,0x0004b9u,0x0004b8u,0x0004b7u,0x0004b6u,0x0004b5u,0x0004b4u,0x0004b3u,
 0x0004b2u,0x0004b1u,0x0004b0u,0x0004afu,0x0004aeu,0x0004adu,0x0004acu,0x0004abu,
 0x0004aau,0x0004a9u,0x0004a8u,0x0004a7u,0x0004a6u,0x0004a5u,0x0004a4u,0x0004a3u,
 0x0004a2u,0x0004a1u,0x0004a0u,0x00049fu,0x00049eu,0x00049du,0x00049cu,0x00049bu,
 0x00049au,0x000499u,0x000498u,0x000497u,0x000496u,0x000495u,0x000494u,0x000493u,
 0x000492u,0x000491u,0x000490u,0x00048fu,0x00048eu,0x00048du,0x00048cu,0x00048bu,
 0x00048au,0x000489u,0x000488u,0x000487u,0x000486u,0x000485u,0x000484u,0x000483u,
 0x000482u,0x000481u,0x000480u,0x00047fu,0x00047eu,0x00047du,0x00047cu,0x00047bu,
 0x00047au,0x000479u,0x000478u,0x000477u,0x000476u,0x000475u,0x000474u,0x000473u,
 0x000472u,0x000471u,0x000470u,0x00046fu,0x00046eu,0x00046du,0x00046cu,0x00046bu,
 0x00046au,0x000469u,0x000468u,0x000467u,0x000466u,0x000465u,0x000464u,0x000463u,
};
static const uint32_t trx_cb_control_word_9[128]={
 0x0004e2u,0x0004e1u,0x0004e0u,0x0004dfu,0x0004deu,0x0004ddu,0x0004dcu,0x0004dbu,
 0x0004dau,0x0004d9u,0x0004d8u,0x0004d7u,0x0004d6u,0x0004d5u,0x0004d4u,0x0004d3u,
 0x0004d2u,0x0004d1u,0x0004d0u,0x0004cfu,0x0004ceu,0x0004cdu,0x0004ccu,0x0004cbu,
 0x0004cau,0x0004c9u,0x0004c8u,0x0004c7u,0x0004c6u,0x0004c5u,0x0004c4u,0x0004c3u,
 0x0004c2u,0x0004c1u,0x0004c0u,0x0004bfu,0x0004beu,0x0004bdu,0x0004bcu,0x0004bbu,
 0x0004bau,0x0004b9u,0x0004b8u,0x0004b7u,0x0004b6u,0x0004b5u,0x0004b4u,0x0004b3u,
 0x0004b2u,0x0004b1u,0x0004b0u,0x0004afu,0x0004aeu,0x0004adu,0x0004acu,0x0004abu,
 0x0004aau,0x0004a9u,0x0004a8u,0x0004a7u,0x0004a6u,0x0004a5u,0x0004a4u,0x0004a3u,
 0x0004a2u,0x0004a1u,0x0004a0u,0x00049fu,0x00049eu,0x00049du,0x00049cu,0x00049bu,
 0x00049au,0x000499u,0x000498u,0x000497u,0x000496u,0x000495u,0x000494u,0x000493u,
 0x000492u,0x000491u,0x000490u,0x00048fu,0x00048eu,0x00048du,0x00048cu,0x00048bu,
 0x00048au,0x000489u,0x000488u,0x000487u,0x000486u,0x000485u,0x000484u,0x000483u,
 0x000482u,0x000481u,0x000480u,0x00047fu,0x00047eu,0x00047du,0x00047cu,0x00047bu,
 0x00047au,0x000479u,0x000478u,0x000477u,0x000476u,0x000475u,0x000474u,0x000473u,
 0x000472u,0x000471u,0x000470u,0x00046fu,0x00046eu,0x00046du,0x00046cu,0x00046bu,
 0x00046au,0x000469u,0x000468u,0x000467u,0x000466u,0x000465u,0x000464u,0x000463u,
};
static const uint32_t trx_cb_control_word_10[128]={
 0x000000u,0x00002au,0x000055u,0x000080u,0x0000aau,0x0000d5u,0x000100u,0x00012au,
 0x000155u,0x000180u,0x0001aau,0x0001d5u,0x000200u,0x00022au,0x000255u,0x000280u,
 0x0002aau,0x0002d5u,0x000300u,0x00032au,0x000355u,0x000380u,0x0003aau,0x0003d5u,
 0x000400u,0x00042au,0x000455u,0x000480u,0x0004aau,0x0004d5u,0x000500u,0x00052au,
 0x000555u,0x000580u,0x0005aau,0x0005d5u,0x000600u,0x00062au,0x000655u,0x000680u,
 0x0006aau,0x0006d5u,0x000700u,0x00072au,0x000755u,0x000780u,0x0007aau,0x0007d5u,
 0x000800u,0x00082au,0x000855u,0x000880u,0x0008aau,0x0008d5u,0x000900u,0x00092au,
 0x000955u,0x000980u,0x0009aau,0x0009d5u,0x000a00u,0x000a2au,0x000a55u,0x000a80u,
 0x000aaau,0x000ad5u,0x000b00u,0x000b2au,0x000b55u,0x000b80u,0x000baau,0x000bd5u,
 0x000c00u,0x000c2au,0x000c55u,0x000c80u,0x000caau,0x000cd5u,0x000d00u,0x000d2au,
 0x000d55u,0x000d80u,0x000daau,0x000dd5u,0x000e00u,0x000e2au,0x000e55u,0x000e80u,
 0x000eaau,0x000ed5u,0x000f00u,0x000f2au,0x000f55u,0x000f80u,0x000faau,0x000fd5u,
 0x001000u,0x00102au,0x001055u,0x001080u,0x0010aau,0x0010d5u,0x001100u,0x00112au,
 0x001155u,0x001180u,0x0011aau,0x0011d5u,0x001200u,0x00122au,0x001255u,0x001280u,
 0x0012aau,0x0012d5u,0x001300u,0x00132au,0x001355u,0x001380u,0x0013aau,0x0013d5u,
 0x001400u,0x00142au,0x001455u,0x001480u,0x0014aau,0x0014d5u,0x001500u,0x00152au,
};
static void trx_cb_trx_cb_from_knobs(const float k[8],uint32_t p[11]){p[0]=trx_cb_control_word_0[(unsigned)k[1]];p[1]=trx_cb_control_word_1[(unsigned)k[5]];p[2]=trx_cb_control_word_2[(unsigned)k[0]];p[3]=trx_cb_control_word_3[(unsigned)k[0]];p[4]=trx_cb_control_word_4[(unsigned)k[4]];p[5]=trx_cb_control_word_5[(unsigned)k[4]];p[6]=trx_cb_control_word_6[(unsigned)k[3]];p[7]=trx_cb_control_word_7[(unsigned)k[3]];p[8]=trx_cb_control_word_8[(unsigned)k[2]];p[9]=trx_cb_control_word_9[(unsigned)k[2]];p[10]=trx_cb_control_word_10[(unsigned)k[5]];}
static void trx_cb_cb_convert(const uint32_t raw[8],uint32_t *p){float k[8];for(unsigned i=0;i<8;++i)k[i]=(float)(raw[i]>>7);trx_cb_trx_cb_from_knobs(k,p);p[2]=711u+raw[0]/97u;p[3]=983u+raw[0]/82u;p[4]=6291455u-raw[4]*256u;p[5]=2097152u+raw[4]*256u;p[6]=p[7]=1966080u+raw[3]*384u;p[10]=raw[5]/3u;}



#ifndef TRX_COMPACT_MODEL_MATH
#define TRX_COMPACT_MODEL_MATH
static inline double trx_cb_trx_model_exp(double x) {
    const double ln2=0.69314718055994530942;
    int n=(int)(x/ln2+(x<0?-0.5:0.5));
    double r=x-n*ln2,term=1,sum=1;
    for(unsigned k=1;k<=18;++k) { term*=r/k;sum+=term; }
    while(n>0) {sum*=2;--n;}
    while(n<0) {sum*=0.5;++n;}
    return sum;
}
static inline double trx_cb_trx_model_cos(double x) {
    double term=1,sum=1;
    for(unsigned k=1;k<=18;++k) {term*=-x*x/((2*k-1)*(2*k));sum+=term;}
    return sum;
}
static inline long long trx_model_round(double x) {return (long long)(x+(x<0?-0.5:0.5));}
static inline uint32_t trx_cb_trx_model_word(double v) {
    if (v > 8388607.0) v = 8388607.0;
    if (v < -8388608.0) v = -8388608.0;
    return (uint32_t)(int32_t)trx_model_round(v) & 0xffffffu;
}
static inline double trx_cb_trx_model_cheb(const double *c,unsigned n,double x) {
    double b1=0,b2=0;
    for(unsigned k=n;k>1;--k) { double b=2*x*b1-b2+c[k-1]; b2=b1;b1=b; }
    return x*b1-b2+c[0];
}
#endif
static const double trx_cb_trx_model_offset_a_c[30]={-232.399680398,153.649305252,-56.7606534091,23.5448649658,-10.6578480114,4.79767127404,-81.056751598,22.7077837039,-2.96244673295,0.299532376803,0.0532892400568,-0.0105212089343,-39.1668372957,14.9894597998,-2.21767366486,0.405478971463,-0.0117738691504,0.138342962517,-15.9882689259,7.39288842873,-1.35951208431,0.190096146761,0.00637058241878,-0.116429618127,-5.5138140884,3.0706297243,-0.547032748071,0.15553066531,0.0144368154093,-0.0443455945788};
static const double trx_cb_trx_model_offset_b_c[13]={-357.643862343,-265.373321533,59.3405978631,-17.7956076723,5.97881763422,-2.11982610249,0.784178870543,-0.267558388544,0.0889657720231,0.0650633677486,0.00493977326511,-0.0123225741715,-0.021290158449};
static inline int trx_model_offset(unsigned i) {
    if(i>=128) return (int)trx_model_round(trx_cb_trx_model_cheb(trx_cb_trx_model_offset_b_c,13,2.0*(i-128)/127.0-1.0));
    unsigned part=i<8?0:i<16?1:i<32?2:i<64?3:4;
    static const unsigned trx_cb_bounds[]={0,8,16,32,64,128};
    unsigned lo=trx_cb_bounds[part],hi=trx_cb_bounds[part+1];
    return (int)trx_model_round(trx_cb_trx_model_cheb(trx_cb_trx_model_offset_a_c+part*6,6,2.0*(i-lo)/(hi-lo-1)-1.0));
}
static inline uint32_t trx_cb_trx_model_memory(unsigned i) {
    if(i<256) return (uint32_t)trx_model_offset(i)&0xffffffu;
    unsigned x=i<4352?i-256:i-4352;
    double t=trx_cb_trx_model_exp(0.003372927270490704*x+(-12.768360569597025));
    return trx_cb_trx_model_word(5931641.6632877039*(i<4352?trx_cb_trx_model_cos(t):trx_cb_trx_model_exp(-t)));
}



static uint32_t trx_cb_metal_norm(unsigned i){return trx_cb_trx_model_memory(i);}
static uint32_t trx_cb_metal_filter(unsigned i){return trx_cb_trx_model_memory(i);}
static uint32_t trx_cb_metal_offsets(unsigned i){return (uint32_t)trx_model_offset(i)&0xffffffu;}








static inline void trx_cb_metal_coeff(unsigned index,unsigned resonance,uint32_t *feedback,uint32_t *square,uint32_t *feed)
{
 uint32_t pole=trx_cb_metal_filter(0x1900+index+trx_cb_md_s24(trx_cb_metal_offsets(128+resonance)));
 uint64_t product=trx_cb_md_mpy(pole,trx_cb_metal_norm(0x900+index+trx_cb_md_s24(trx_cb_metal_offsets(resonance))));
 *square=trx_cb_md_acc_to_word(trx_cb_md_sub(0,trx_cb_md_mpy(pole,pole)));
 *feedback=trx_cb_md_acc_to_word(trx_cb_md_asl(product,1));
 *feed=trx_cb_md_acc_to_word(trx_cb_md_sub(trx_cb_md_from24(0x400000),trx_cb_md_add(trx_cb_md_asl(product,1),trx_cb_md_from24(*square))));
}





static inline void trx_cb_metal_difference_cascade(uint32_t*w,unsigned input_base,unsigned output_base,unsigned history_base,uint32_t history[6],unsigned index,unsigned resonance)
{
 uint32_t feedback,square,unused;trx_cb_metal_coeff(index,resonance,&feedback,&square,&unused);
 w[history_base]=history[1];w[history_base+1]=history[0];
 w[output_base]=history[3];w[output_base+1]=history[2];
 w[input_base-2]=history[5];w[input_base-1]=history[4];
 unsigned memory=0,input=input_base-2,delay=output_base+1;
 uint64_t first=trx_cb_md_mpy(square,w[history_base+memory]);memory^=1;
 first=trx_cb_md_mac(first,feedback,w[history_base+memory]);
 uint32_t sample=w[input++];
 for(unsigned i=0;i<32;++i){
  first=trx_cb_md_mac(first,sample,0x400000);sample=w[input++];
  first=trx_cb_md_mac(first,sample,0x800000);sample=w[input--];
  first=trx_cb_md_mac(first,sample,0x400000);
  first=trx_cb_md_asl(first,1);
  --delay;
  uint64_t second=trx_cb_md_mpy(square,w[delay++]);
  second=trx_cb_md_mac(second,feedback,w[delay++]);
  uint32_t first_word=trx_cb_md_acc_to_word(first);
  second=trx_cb_md_mac(second,first_word,0x400000);
  second=trx_cb_md_mac(second,w[history_base+memory],0x800000);memory^=1;
  second=trx_cb_md_mac(second,w[history_base+memory],0x400000);
  w[history_base+memory]=first_word;memory^=1;
  second=trx_cb_md_asl(second,1);
  first=trx_cb_md_mpy(square,w[history_base+memory]);memory^=1;
  sample=w[input++];first=trx_cb_md_mac(first,first_word,feedback);
  w[delay]=trx_cb_md_acc_to_word(second);
 }
 history[0]=w[history_base+memory];memory^=1;history[1]=w[history_base+memory];
 history[2]=w[delay];history[3]=w[delay-1];
 history[4]=w[input];history[5]=w[input-1];
}







static void trx_cb_clear_state(void *memory,unsigned bytes)
{
 volatile unsigned char *cursor=memory;
 while(bytes--)*cursor++=0;
}
static void trx_cb_cb_controls(mdkit_trx_cb*s)
{uint32_t raw[8],p[64];for(unsigned i=0;i<8;++i)raw[i]=(uint32_t)(128*s->clock.current[i]);trx_cb_cb_convert(raw,p);s->decay=p[0];s->bump_decay=p[1];for(unsigned i=0;i<2;++i){s->pitch[i]=p[2+i];s->gain[i]=p[4+i];s->clip[i]=p[6+i];s->filter_index[i]=p[8+i];}s->bump=p[10];}

static void trx_cb_cb_init(void*state)
{mdkit_trx_cb*s=state;trx_cb_clear_state(s,sizeof*s);trx_cb_md_trx_events_init(&s->clock,&s->events);trx_cb_cb_controls(s);}
static void trx_cb_cb_set(void*state,unsigned i,unsigned value){trx_cb_md_trx_events_target(&((mdkit_trx_cb*)state)->events,6,i,value);}
static void trx_cb_cb_trigger(void*state){((mdkit_trx_cb*)state)->events.trigger=1;}
static void trx_cb_cb_boundary(mdkit_trx_cb*s)
{unsigned flags=trx_cb_md_trx_events_boundary(&s->clock,&s->events,0);if(flags&trx_cb_MD_TRX_PUBLISH){uint32_t bump=s->bump;trx_cb_cb_controls(s);if(!(flags&trx_cb_MD_TRX_TRIGGER))s->bump=bump;}if(flags&trx_cb_MD_TRX_FIRST){s->phase[0]=s->phase[1]=0;trx_cb_clear_state(s->history,sizeof s->history);}if(flags&trx_cb_MD_TRX_TRIGGER){s->envelope=trx_cb_md_acc_to_word(trx_cb_md_add(trx_cb_md_from24(0x50c335),trx_cb_md_from24(s->envelope)));s->delay=3;}}
static void trx_cb_cb_oscillator(mdkit_trx_cb*s,unsigned osc,uint32_t bump)
{
 uint64_t step=osc?trx_cb_md_add(trx_cb_md_from24(s->pitch[osc]),trx_cb_md_from24(bump)):trx_cb_md_sub(trx_cb_md_from24(s->pitch[osc]),trx_cb_md_from24(bump));step=trx_cb_md_asr(step,1);
 uint64_t phase=trx_cb_md_from48(s->phase[osc]);unsigned base=osc?96:32;
 for(unsigned i=0;i<32;++i){uint32_t original=trx_cb_local_sine(trx_cb_md_mid(phase)&0x7fff),wave=original;phase=trx_cb_md_and_mid(trx_cb_md_add(phase,step),0x7fff);
 if(trx_cb_md_s24(wave)>trx_cb_md_s24(s->clip[osc]))wave=0x7ffcb9;
 if(-trx_cb_md_s24(original)>trx_cb_md_s24(s->clip[osc]))wave=0x800347;
 s->workspace[base+i]=wave;}
 s->phase[osc]=trx_cb_md_pack48(trx_cb_md_mid(phase),trx_cb_md_low(phase));
}
static void trx_cb_cb_block(mdkit_trx_cb*s)
{
 if(s->delay){--s->delay;for(unsigned i=0;i<32;++i)s->block[i]=0;if(s->delay>1){uint32_t value=s->last_output;for(unsigned i=0;i<32;++i){value=trx_cb_md_acc_to_word(trx_cb_md_mpy(value,0x733333));s->block[i]=value;}}return;}
 uint32_t bump=s->bump;trx_cb_cb_oscillator(s,0,bump);trx_cb_cb_oscillator(s,1,bump);s->bump=trx_cb_md_acc_to_word(trx_cb_md_mpy(s->bump_decay,bump));
 uint32_t gain[2];for(unsigned i=0;i<2;++i)gain[i]=trx_cb_md_acc_to_word(trx_cb_md_mpy(s->gain[i],s->envelope));
 for(unsigned i=0;i<32;++i)for(unsigned osc=0;osc<2;++osc){unsigned base=osc?96:32;s->workspace[base+i]=trx_cb_md_acc_to_word(trx_cb_md_mpy(gain[osc],s->workspace[base+i]));}
 s->envelope=trx_cb_md_acc_to_word(trx_cb_md_mpy(s->decay,s->envelope));
 trx_cb_metal_difference_cascade(s->workspace,32,0,64,s->history[0],s->filter_index[0],127);
 trx_cb_metal_difference_cascade(s->workspace,96,64,32,s->history[1],s->filter_index[1],127);
 for(unsigned i=0;i<32;++i)s->block[i]=trx_cb_md_acc_to_word(trx_cb_md_asr(trx_cb_md_add(trx_cb_md_from24(s->workspace[i]),trx_cb_md_from24(s->workspace[64+i])),2));
 s->last_output=s->block[31];
}
static void trx_cb_cb_render(void*state,float*out,unsigned frames)
{mdkit_trx_cb*s=state;while(frames){if(!s->pos){trx_cb_cb_boundary(s);trx_cb_cb_block(s);}unsigned n=32-s->pos;if(n>frames)n=frames;for(unsigned i=0;i<n;++i)out[i]=(float)trx_cb_md_s24(s->block[s->pos+i])/8388608;frames-=n;out+=n;s->pos=(s->pos+n)&31;}}
static const char*const trx_cb_names[]={"PTCH","DEC","ENH","DAMP","TONE","BUMP"};
const mdkit_voice mdkit_voice_trx_cb={"TRX-CB",21,6,trx_cb_names,sizeof(mdkit_trx_cb),trx_cb_cb_init,trx_cb_cb_set,trx_cb_cb_trigger,trx_cb_cb_render, mdkit_trx_cb_defaults
};
