/* SPDX-License-Identifier: CC0-1.0
 * TRX-B2: self-contained drum synthesis.
 */
#include "mdkit_trx_b2.h"

/* ---- Default controls --------------------------------------------------- */
/* Active controls are in descriptor order; unused clock slots stay at 64. */
static const unsigned char mdkit_trx_b2_defaults[MDKIT_MAX_CONTROLS] = {
    60, 65, 41, 62, 91, 44, 56, 58
};


#define MD_MASK24 UINT32_C(0xffffff)
#define MD_MASK48 UINT64_C(0xffffffffffff)
#define MD_MASK56 UINT64_C(0xffffffffffffff)

static inline int64_t b2_signed_bits(uint64_t word, unsigned bits)
{
    const uint64_t sign = UINT64_C(1) << (bits - 1);
    const uint64_t mask = (UINT64_C(1) << bits) - 1;
    word &= mask;
    return (word & sign) ? (int64_t)word - (int64_t)(UINT64_C(1) << bits)
                         : (int64_t)word;
}
static inline int64_t b2_s24(uint32_t w) { return b2_signed_bits(w & MD_MASK24, 24); }
static inline int64_t b2_s56(uint64_t a) { return b2_signed_bits(a & MD_MASK56, 56); }
static inline uint32_t b2_mid(uint64_t a) { return (uint32_t)((a >> 24) & MD_MASK24); }

static inline uint64_t b2_from24(uint32_t w)
{ return ((uint64_t)(b2_s24(w) * (INT64_C(1) << 24))) & MD_MASK56; }


static inline uint64_t b2_setmid(uint64_t a, uint32_t w)
{ return (a & ~(UINT64_C(0xffffff) << 24)) | ((uint64_t)(w & MD_MASK24) << 24); }
static inline uint64_t b2_add(uint64_t a, uint64_t b) { return (a + b) & MD_MASK56; }
static inline uint64_t b2_sub(uint64_t a, uint64_t b) { return (a - b) & MD_MASK56; }
static inline uint64_t b2_asl(uint64_t a, unsigned n) { return (a << n) & MD_MASK56; }
static inline uint64_t b2_asr(uint64_t a, unsigned n)
{
    const int64_t v = b2_s56(a);
    const int64_t sh = v >= 0 ? v / (INT64_C(1) << n)
        : -((-v + (INT64_C(1) << n) - 1) / (INT64_C(1) << n));
    return (uint64_t)sh & MD_MASK56;
}
static inline uint64_t b2_mpy(uint32_t a, uint32_t b)
{ return (uint64_t)(b2_s24(a) * b2_s24(b) * 2) & MD_MASK56; }




static inline uint64_t b2_mac(uint64_t acc, uint32_t x, uint32_t y)
{
    return (acc + b2_mpy(x, y)) & MD_MASK56;
}


static inline uint64_t b2_and_mid(uint64_t a, uint32_t mask)
{ return b2_setmid(a, b2_mid(a) & mask); }

static inline uint32_t b2_acc_to_word(uint64_t a)
{
    const int64_t v = b2_s56(a);
    if(v < -(INT64_C(0x800000) << 24)) return UINT32_C(0x800000);
    if(v >  (INT64_C(0x7fffff) << 24)) return UINT32_C(0x7fffff);
    return b2_mid(a);
}

static inline void b2_control_clock_seed(mdkit_control_clock *c, unsigned count,
                                         const unsigned char *knobs)
{
    c->count = count;
    for (unsigned i = 0; i < count; ++i) {
        const float v = (float)knobs[i];
        c->target[i] = c->raw[i] = c->current[i] = c->pending[i] = v;
    }
    c->block = 0; c->next_update = 0; c->publish_block = 0; c->pending_valid = 0;
}


static double b2_local_sin(double x)
{
    x -= 6.283185307179586476925286766559 * (double)(long long)(x / 6.283185307179586476925286766559
                                            + (x < 0.0 ? -0.5 : 0.5));
    if (x > 3.14159265358979323846 / 2.0) x = 3.14159265358979323846 - x;
    else if (x < -3.14159265358979323846 / 2.0) x = -3.14159265358979323846 - x;
    const double x2 = x * x;
    return x * (1.0 - x2 * (1.0 / 6.0 - x2 * (1.0 / 120.0 - x2 * (1.0 / 5040.0
        - x2 * (1.0 / 362880.0 - x2 * (1.0 / 39916800.0))))));
}

static double b2_local_exp2(double x)
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


static uint32_t b2_local_sine(uint16_t index){double y=8388608.0*b2_local_sin(6.283185307179586476925286766559*(double)(index&32767u)/32768.0);int64_t v=(int64_t)(y+(y<0?-0.5:0.5));if(v>8388607)v=8388607;if(v< -8388608)v=-8388608;return (uint32_t)v&MD_MASK24;}

static const uint32_t b2_trx_b2_PTCH_cc128[128] = {
0x00076eu,0x0007d1u,0x000834u,0x0008a0u,0x000903u,0x000966u,0x0009d2u,0x000a35u,
0x000aa1u,0x000b0du,0x000b70u,0x000bdcu,0x000c48u,0x000cb4u,0x000d20u,0x000d8cu,
0x000df8u,0x000e64u,0x000ed9u,0x000f45u,0x000fb1u,0x001026u,0x001092u,0x001107u,
0x001173u,0x0011e8u,0x00125du,0x0012d2u,0x001347u,0x0013bcu,0x001431u,0x0014a6u,
0x00151bu,0x001590u,0x00160eu,0x001683u,0x001701u,0x001776u,0x0017f4u,0x001872u,
0x0018e7u,0x001965u,0x0019e3u,0x001a61u,0x001adfu,0x001b66u,0x001be4u,0x001c62u,
0x001ce9u,0x001d67u,0x001deeu,0x001e6cu,0x001ef3u,0x001f7au,0x002001u,0x002088u,
0x00210fu,0x002196u,0x00221du,0x0022a4u,0x002334u,0x0023bbu,0x00244bu,0x0024dbu,
0x002562u,0x0025f2u,0x002682u,0x002712u,0x0027a2u,0x002832u,0x0028c2u,0x00295bu,
0x0029ebu,0x002a84u,0x002b14u,0x002badu,0x002c46u,0x002cdfu,0x002d78u,0x002e11u,
0x002eaau,0x002f43u,0x002fdcu,0x00307eu,0x003117u,0x0031b9u,0x00325bu,0x0032fdu,
0x003396u,0x003438u,0x0034e3u,0x003585u,0x003627u,0x0036d2u,0x003774u,0x00381fu,
0x0038c1u,0x00396cu,0x003a17u,0x003ac2u,0x003b6du,0x003c18u,0x003cccu,0x003d77u,
0x003e2bu,0x003ed6u,0x003f8au,0x00403eu,0x0040f2u,0x0041a6u,0x00425au,0x004317u,
0x0043cbu,0x004488u,0x00453cu,0x0045f9u,0x0046b6u,0x004773u,0x004830u,0x0048edu,
0x0049aau,0x004a70u,0x004b2du,0x004bf3u,0x004cb9u,0x004d7fu,0x004e45u,0x004f0bu,
};
static const uint32_t b2_trx_b2_RAMP_cc128[128] = {
0x000000u,0x000252u,0x0004a4u,0x0006f7u,0x000949u,0x000b9cu,0x000deeu,0x001041u,
0x001293u,0x0014e5u,0x001738u,0x00198au,0x001bddu,0x001e2fu,0x002082u,0x0022d4u,
0x002526u,0x002779u,0x0029cbu,0x002c1eu,0x002e70u,0x0030c3u,0x003315u,0x003567u,
0x0037bau,0x003a0cu,0x003c5fu,0x003eb1u,0x004104u,0x004356u,0x0045a8u,0x0047fbu,
0x004a4du,0x004ca0u,0x004ef2u,0x005145u,0x005397u,0x0055e9u,0x00583cu,0x005a8eu,
0x005ce1u,0x005f33u,0x006186u,0x0063d8u,0x00662au,0x00687du,0x006acfu,0x006d22u,
0x006f74u,0x0071c7u,0x007419u,0x00766bu,0x0078beu,0x007b10u,0x007d63u,0x007fb5u,
0x008208u,0x00845au,0x0086acu,0x0088ffu,0x008b51u,0x008da4u,0x008ff6u,0x009249u,
0x00949bu,0x0096edu,0x009940u,0x009b92u,0x009de5u,0x00a037u,0x00a28au,0x00a4dcu,
0x00a72eu,0x00a981u,0x00abd3u,0x00ae26u,0x00b078u,0x00b2cbu,0x00b51du,0x00b76fu,
0x00b9c2u,0x00bc14u,0x00be67u,0x00c0b9u,0x00c30cu,0x00c55eu,0x00c7b0u,0x00ca03u,
0x00cc55u,0x00cea8u,0x00d0fau,0x00d34du,0x00d59fu,0x00d7f1u,0x00da44u,0x00dc96u,
0x00dee9u,0x00e13bu,0x00e38eu,0x00e5e0u,0x00e832u,0x00ea85u,0x00ecd7u,0x00ef2au,
0x00f17cu,0x00f3cfu,0x00f621u,0x00f873u,0x00fac6u,0x00fd18u,0x00ff6bu,0x0101bdu,
0x010410u,0x010662u,0x0108b4u,0x010b07u,0x010d59u,0x010facu,0x0111feu,0x011451u,
0x0116a3u,0x0118f5u,0x011b48u,0x011d9au,0x011fedu,0x01223fu,0x012492u,0x0126e4u,
};
static const uint32_t b2_trx_b2_DEC_cc128[128] = {
0x7c6d17u,0x7caa82u,0x7ce3dbu,0x7d1965u,0x7d4b60u,0x7d7a06u,0x7da58fu,0x7dce30u,
0x7df419u,0x7e1778u,0x7e3878u,0x7e5742u,0x7e73f9u,0x7e8ec3u,0x7ea7bfu,0x7ebf0du,
0x7ed4c8u,0x7ee90du,0x7efbf3u,0x7f0d93u,0x7f1e02u,0x7f2d55u,0x7f3b9fu,0x7f48f1u,
0x7f555du,0x7f60f1u,0x7f6bbdu,0x7f75cdu,0x7f7f2fu,0x7f87eeu,0x7f9016u,0x7f97b0u,
0x7f9ec5u,0x7fa560u,0x7fab88u,0x7fb146u,0x7fb69fu,0x7fbb9cu,0x7fc042u,0x7fc497u,
0x7fc8a0u,0x7fcc64u,0x7fcfe6u,0x7fd32bu,0x7fd637u,0x7fd90fu,0x7fdbb4u,0x7fde2cu,
0x7fe079u,0x7fe29eu,0x7fe49du,0x7fe67au,0x7fe836u,0x7fe9d4u,0x7feb56u,0x7fecbeu,
0x7fee0du,0x7fef46u,0x7ff069u,0x7ff178u,0x7ff275u,0x7ff361u,0x7ff43du,0x7ff50au,
0x7ff5c9u,0x7ff67au,0x7ff720u,0x7ff7bbu,0x7ff84bu,0x7ff8d1u,0x7ff94eu,0x7ff9c3u,
0x7ffa2fu,0x7ffa95u,0x7ffaf3u,0x7ffb4bu,0x7ffb9du,0x7ffbe9u,0x7ffc30u,0x7ffc73u,
0x7ffcb1u,0x7ffceau,0x7ffd20u,0x7ffd52u,0x7ffd81u,0x7ffdacu,0x7ffdd5u,0x7ffdfbu,
0x7ffe1eu,0x7ffe3fu,0x7ffe5du,0x7ffe7au,0x7ffe94u,0x7ffeadu,0x7ffec4u,0x7ffed9u,
0x7ffeeeu,0x7fff00u,0x7fff12u,0x7fff22u,0x7fff31u,0x7fff3fu,0x7fff4cu,0x7fff58u,
0x7fff64u,0x7fff6eu,0x7fff78u,0x7fff82u,0x7fff8au,0x7fff92u,0x7fff9au,0x7fffa1u,
0x7fffa7u,0x7fffadu,0x7fffb3u,0x7fffb8u,0x7fffbdu,0x7fffc1u,0x7fffc6u,0x7fffcau,
0x7fffcdu,0x7fffd1u,0x7fffd4u,0x7fffd7u,0x7fffdau,0x7fffdcu,0x7fffdfu,0x7fffe1u,
};
static const uint32_t b2_trx_b2_HOLD_cc128[128] = {
0x2fac4cu,0x329e5du,0x358d09u,0x3875b3u,0x3b55f7u,0x3e2ba9u,0x40f4dau,0x43afd6u,
0x465b20u,0x48f576u,0x4b7dc8u,0x4df33bu,0x505522u,0x52a2fbu,0x54dc6cu,0x570140u,
0x591163u,0x5b0cddu,0x5cf3d2u,0x5ec67au,0x608524u,0x62302du,0x63c802u,0x654d1cu,
0x66bffdu,0x68212fu,0x697143u,0x6ab0cbu,0x6be05fu,0x6d0099u,0x6e1211u,0x6f1560u,
0x700b1eu,0x70f3e0u,0x71d039u,0x72a0bau,0x7365eeu,0x74205fu,0x74d091u,0x757705u,
0x761436u,0x76a89bu,0x7734a9u,0x77b8cdu,0x783570u,0x78aaf9u,0x7919c9u,0x79823au,
0x79e4a7u,0x7a4163u,0x7a98bfu,0x7aeb05u,0x7b3880u,0x7b8173u,0x7bc61fu,0x7c06c3u,
0x7c439au,0x7c7cdau,0x7cb2b9u,0x7ce567u,0x7d1515u,0x7d41efu,0x7d6c1eu,0x7d93cau,
0x7db91au,0x7ddc30u,0x7dfd2du,0x7e1c32u,0x7e395cu,0x7e54c6u,0x7e6e8cu,0x7e86c7u,
0x7e9d8du,0x7eb2f5u,0x7ec713u,0x7ed9fcu,0x7eebc2u,0x7efc75u,0x7f0c27u,0x7f1ae7u,
0x7f28c4u,0x7f35cau,0x7f4207u,0x7f4d87u,0x7f5855u,0x7f627cu,0x7f6c07u,0x7f74fdu,
0x7f7d69u,0x7f8552u,0x7f8cc1u,0x7f93bdu,0x7f9a4du,0x7fa077u,0x7fa641u,0x7fabb2u,
0x7fb0ceu,0x7fb59cu,0x7fba1eu,0x7fbe5bu,0x7fc256u,0x7fc614u,0x7fc997u,0x7fcce4u,
0x7fcffdu,0x7fd2e7u,0x7fd5a3u,0x7fd835u,0x7fda9fu,0x7fdce3u,0x7fdf05u,0x7fe105u,
0x7fe2e6u,0x7fe4aau,0x7fe653u,0x7fe7e1u,0x7fe958u,0x7feab8u,0x7fec02u,0x7fed39u,
0x7fee5du,0x7fef6fu,0x7ff070u,0x7ff162u,0x7ff245u,0x7ff31au,0x7ff3e2u,0x7ff49fu,
};
static const uint32_t b2_trx_b2_DIST0_cc128[128] = {
0xff8000u,0xff7f10u,0xff7c40u,0xff7790u,0xff7100u,0xff6890u,0xff5e40u,0xff5210u,
0xff4400u,0xff3410u,0xff2240u,0xff0e90u,0xfef900u,0xfee190u,0xfec840u,0xfead10u,
0xfe9000u,0xfe7110u,0xfe5040u,0xfe2d90u,0xfe0900u,0xfde290u,0xfdba40u,0xfd9010u,
0xfd6400u,0xfd3610u,0xfd0640u,0xfcd490u,0xfca100u,0xfc6b90u,0xfc3440u,0xfbfb10u,
0xfbc000u,0xfb8310u,0xfb4440u,0xfb0390u,0xfac100u,0xfa7c90u,0xfa3640u,0xf9ee10u,
0xf9a400u,0xf95810u,0xf90a40u,0xf8ba90u,0xf86900u,0xf81590u,0xf7c040u,0xf76910u,
0xf71000u,0xf6b510u,0xf65840u,0xf5f990u,0xf59900u,0xf53690u,0xf4d240u,0xf46c10u,
0xf40400u,0xf39a10u,0xf32e40u,0xf2c090u,0xf25100u,0xf1df90u,0xf16c40u,0xf0f710u,
0xf08000u,0xf00710u,0xef8c40u,0xef0f90u,0xee9100u,0xee1090u,0xed8e40u,0xed0a10u,
0xec8400u,0xebfc10u,0xeb7240u,0xeae690u,0xea5900u,0xe9c990u,0xe93840u,0xe8a510u,
0xe81000u,0xe77910u,0xe6e040u,0xe64590u,0xe5a900u,0xe50a90u,0xe46a40u,0xe3c810u,
0xe32400u,0xe27e10u,0xe1d640u,0xe12c90u,0xe08100u,0xdfd390u,0xdf2440u,0xde7310u,
0xddc000u,0xdd0b10u,0xdc5440u,0xdb9b90u,0xdae100u,0xda2490u,0xd96640u,0xd8a610u,
0xd7e400u,0xd72010u,0xd65a40u,0xd59290u,0xd4c900u,0xd3fd90u,0xd33040u,0xd26110u,
0xd19000u,0xd0bd10u,0xcfe840u,0xcf1190u,0xce3900u,0xcd5e90u,0xcc8240u,0xcba410u,
0xcac400u,0xc9e210u,0xc8fe40u,0xc81890u,0xc73100u,0xc64790u,0xc55c40u,0xc46f10u,
};
static const uint32_t b2_trx_b2_DIST1_cc128[128] = {
0x008000u,0x008118u,0x008460u,0x0089d8u,0x009180u,0x009b58u,0x00a760u,0x00b598u,
0x00c600u,0x00d898u,0x00ed60u,0x010458u,0x011d80u,0x0138d8u,0x015660u,0x017618u,
0x019800u,0x01bc18u,0x01e260u,0x020ad8u,0x023580u,0x026258u,0x029160u,0x02c298u,
0x02f600u,0x032b98u,0x036360u,0x039d58u,0x03d980u,0x0417d8u,0x045860u,0x049b18u,
0x04e000u,0x052718u,0x057060u,0x05bbd8u,0x060980u,0x065958u,0x06ab60u,0x06ff98u,
0x075600u,0x07ae98u,0x080960u,0x086658u,0x08c580u,0x0926d8u,0x098a60u,0x09f018u,
0x0a5800u,0x0ac218u,0x0b2e60u,0x0b9cd8u,0x0c0d80u,0x0c8058u,0x0cf560u,0x0d6c98u,
0x0de600u,0x0e6198u,0x0edf60u,0x0f5f58u,0x0fe180u,0x1065d8u,0x10ec60u,0x117518u,
0x120000u,0x128d18u,0x131c60u,0x13add8u,0x144180u,0x14d758u,0x156f60u,0x160998u,
0x16a600u,0x174498u,0x17e560u,0x188858u,0x192d80u,0x19d4d8u,0x1a7e60u,0x1b2a18u,
0x1bd800u,0x1c8818u,0x1d3a60u,0x1deed8u,0x1ea580u,0x1f5e58u,0x201960u,0x20d698u,
0x219600u,0x225798u,0x231b60u,0x23e158u,0x24a980u,0x2573d8u,0x264060u,0x270f18u,
0x27e000u,0x28b318u,0x298860u,0x2a5fd8u,0x2b3980u,0x2c1558u,0x2cf360u,0x2dd398u,
0x2eb600u,0x2f9a98u,0x308160u,0x316a58u,0x325580u,0x3342d8u,0x343260u,0x352418u,
0x361800u,0x370e18u,0x380660u,0x3900d8u,0x39fd80u,0x3afc58u,0x3bfd60u,0x3d0098u,
0x3e0600u,0x3f0d98u,0x401760u,0x412358u,0x423180u,0x4341d8u,0x445460u,0x456918u,
};
static const uint32_t b2_trx_b2_DIST2_cc128[128] = {
0xff8000u,0xff7f38u,0xff7ce0u,0xff78f8u,0xff7380u,0xff6c78u,0xff63e0u,0xff59b8u,
0xff4e00u,0xff40b8u,0xff31e0u,0xff2178u,0xff0f80u,0xfefbf8u,0xfee6e0u,0xfed038u,
0xfeb800u,0xfe9e38u,0xfe82e0u,0xfe65f8u,0xfe4780u,0xfe2778u,0xfe05e0u,0xfde2b8u,
0xfdbe00u,0xfd97b8u,0xfd6fe0u,0xfd4678u,0xfd1b80u,0xfceef8u,0xfcc0e0u,0xfc9138u,
0xfc6000u,0xfc2d38u,0xfbf8e0u,0xfbc2f8u,0xfb8b80u,0xfb5278u,0xfb17e0u,0xfadbb8u,
0xfa9e00u,0xfa5eb8u,0xfa1de0u,0xf9db78u,0xf99780u,0xf951f8u,0xf90ae0u,0xf8c238u,
0xf87800u,0xf82c38u,0xf7dee0u,0xf78ff8u,0xf73f80u,0xf6ed78u,0xf699e0u,0xf644b8u,
0xf5ee00u,0xf595b8u,0xf53be0u,0xf4e078u,0xf48380u,0xf424f8u,0xf3c4e0u,0xf36338u,
0xf30000u,0xf29b38u,0xf234e0u,0xf1ccf8u,0xf16380u,0xf0f878u,0xf08be0u,0xf01db8u,
0xefae00u,0xef3cb8u,0xeec9e0u,0xee5578u,0xeddf80u,0xed67f8u,0xeceee0u,0xec7438u,
0xebf800u,0xeb7a38u,0xeafae0u,0xea79f8u,0xe9f780u,0xe97378u,0xe8ede0u,0xe866b8u,
0xe7de00u,0xe753b8u,0xe6c7e0u,0xe63a78u,0xe5ab80u,0xe51af8u,0xe488e0u,0xe3f538u,
0xe36000u,0xe2c938u,0xe230e0u,0xe196f8u,0xe0fb80u,0xe05e78u,0xdfbfe0u,0xdf1fb8u,
0xde7e00u,0xdddab8u,0xdd35e0u,0xdc8f78u,0xdbe780u,0xdb3df8u,0xda92e0u,0xd9e638u,
0xd93800u,0xd88838u,0xd7d6e0u,0xd723f8u,0xd66f80u,0xd5b978u,0xd501e0u,0xd448b8u,
0xd38e00u,0xd2d1b8u,0xd213e0u,0xd15478u,0xd09380u,0xcfd0f8u,0xcf0ce0u,0xce4738u,
};
static const uint32_t b2_trx_b2_DIST3_cc128[128] = {
0x008000u,0x0080a0u,0x008280u,0x0085a0u,0x008a00u,0x008fa0u,0x009680u,0x009ea0u,
0x00a800u,0x00b2a0u,0x00be80u,0x00cba0u,0x00da00u,0x00e9a0u,0x00fa80u,0x010ca0u,
0x012000u,0x0134a0u,0x014a80u,0x0161a0u,0x017a00u,0x0193a0u,0x01ae80u,0x01caa0u,
0x01e800u,0x0206a0u,0x022680u,0x0247a0u,0x026a00u,0x028da0u,0x02b280u,0x02d8a0u,
0x030000u,0x0328a0u,0x035280u,0x037da0u,0x03aa00u,0x03d7a0u,0x040680u,0x0436a0u,
0x046800u,0x049aa0u,0x04ce80u,0x0503a0u,0x053a00u,0x0571a0u,0x05aa80u,0x05e4a0u,
0x062000u,0x065ca0u,0x069a80u,0x06d9a0u,0x071a00u,0x075ba0u,0x079e80u,0x07e2a0u,
0x082800u,0x086ea0u,0x08b680u,0x08ffa0u,0x094a00u,0x0995a0u,0x09e280u,0x0a30a0u,
0x0a8000u,0x0ad0a0u,0x0b2280u,0x0b75a0u,0x0bca00u,0x0c1fa0u,0x0c7680u,0x0ccea0u,
0x0d2800u,0x0d82a0u,0x0dde80u,0x0e3ba0u,0x0e9a00u,0x0ef9a0u,0x0f5a80u,0x0fbca0u,
0x102000u,0x1084a0u,0x10ea80u,0x1151a0u,0x11ba00u,0x1223a0u,0x128e80u,0x12faa0u,
0x136800u,0x13d6a0u,0x144680u,0x14b7a0u,0x152a00u,0x159da0u,0x161280u,0x1688a0u,
0x170000u,0x1778a0u,0x17f280u,0x186da0u,0x18ea00u,0x1967a0u,0x19e680u,0x1a66a0u,
0x1ae800u,0x1b6aa0u,0x1bee80u,0x1c73a0u,0x1cfa00u,0x1d81a0u,0x1e0a80u,0x1e94a0u,
0x1f2000u,0x1faca0u,0x203a80u,0x20c9a0u,0x215a00u,0x21eba0u,0x227e80u,0x2312a0u,
0x23a800u,0x243ea0u,0x24d680u,0x256fa0u,0x260a00u,0x26a5a0u,0x274280u,0x27e0a0u,
};
static const uint32_t b2_trx_b2_DIRT_SHIFT_cc128[128] = {
0x000000u,0x000003u,0x000005u,0x000006u,0x000007u,0x000008u,0x000009u,0x00000au,
0x00000au,0x00000bu,0x00000bu,0x00000cu,0x00000cu,0x00000du,0x00000du,0x00000du,
0x00000eu,0x00000eu,0x00000eu,0x00000eu,0x00000fu,0x00000fu,0x00000fu,0x00000fu,
0x00000fu,0x000010u,0x000010u,0x000010u,0x000010u,0x000010u,0x000010u,0x000011u,
0x000011u,0x000011u,0x000011u,0x000011u,0x000011u,0x000011u,0x000012u,0x000012u,
0x000012u,0x000012u,0x000012u,0x000012u,0x000012u,0x000012u,0x000013u,0x000013u,
0x000013u,0x000013u,0x000013u,0x000013u,0x000013u,0x000013u,0x000013u,0x000013u,
0x000013u,0x000014u,0x000014u,0x000014u,0x000014u,0x000014u,0x000014u,0x000014u,
0x000014u,0x000014u,0x000014u,0x000014u,0x000014u,0x000015u,0x000015u,0x000015u,
0x000015u,0x000015u,0x000015u,0x000015u,0x000015u,0x000015u,0x000015u,0x000015u,
0x000015u,0x000015u,0x000015u,0x000015u,0x000015u,0x000016u,0x000016u,0x000016u,
0x000016u,0x000016u,0x000016u,0x000016u,0x000016u,0x000016u,0x000016u,0x000016u,
0x000016u,0x000016u,0x000016u,0x000016u,0x000016u,0x000016u,0x000016u,0x000016u,
0x000017u,0x000017u,0x000017u,0x000017u,0x000017u,0x000017u,0x000017u,0x000017u,
0x000017u,0x000017u,0x000017u,0x000017u,0x000017u,0x000017u,0x000017u,0x000017u,
0x000017u,0x000017u,0x000017u,0x000017u,0x000017u,0x000017u,0x000017u,0x000017u,
};
static const uint32_t b2_trx_b2_DIRT_BIAS_cc128[128] = {
0x000000u,0x000004u,0x000010u,0x000020u,0x000040u,0x000080u,0x000100u,0x000200u,
0x000200u,0x000400u,0x000400u,0x000800u,0x000800u,0x001000u,0x001000u,0x001000u,
0x002000u,0x002000u,0x002000u,0x002000u,0x004000u,0x004000u,0x004000u,0x004000u,
0x004000u,0x008000u,0x008000u,0x008000u,0x008000u,0x008000u,0x008000u,0x010000u,
0x010000u,0x010000u,0x010000u,0x010000u,0x010000u,0x010000u,0x020000u,0x020000u,
0x020000u,0x020000u,0x020000u,0x020000u,0x020000u,0x020000u,0x040000u,0x040000u,
0x040000u,0x040000u,0x040000u,0x040000u,0x040000u,0x040000u,0x040000u,0x040000u,
0x040000u,0x080000u,0x080000u,0x080000u,0x080000u,0x080000u,0x080000u,0x080000u,
0x080000u,0x080000u,0x080000u,0x080000u,0x080000u,0x100000u,0x100000u,0x100000u,
0x100000u,0x100000u,0x100000u,0x100000u,0x100000u,0x100000u,0x100000u,0x100000u,
0x100000u,0x100000u,0x100000u,0x100000u,0x100000u,0x200000u,0x200000u,0x200000u,
0x200000u,0x200000u,0x200000u,0x200000u,0x200000u,0x200000u,0x200000u,0x200000u,
0x200000u,0x200000u,0x200000u,0x200000u,0x200000u,0x200000u,0x200000u,0x200000u,
0x400000u,0x400000u,0x400000u,0x400000u,0x400000u,0x400000u,0x400000u,0x400000u,
0x400000u,0x400000u,0x400000u,0x400000u,0x400000u,0x400000u,0x400000u,0x400000u,
0x400000u,0x400000u,0x400000u,0x400000u,0x400000u,0x400000u,0x400000u,0x400000u,
};
static const uint32_t b2_trx_b2_NOIS_cc128[128] = {
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
static const uint32_t b2_trx_b2_TICK_cc128[128] = {
0x000000u,0x008000u,0x010000u,0x018000u,0x020000u,0x028000u,0x030000u,0x038000u,
0x040000u,0x048000u,0x050000u,0x058000u,0x060000u,0x068000u,0x070000u,0x078000u,
0x080000u,0x088000u,0x090000u,0x098000u,0x0a0000u,0x0a8000u,0x0b0000u,0x0b8000u,
0x0c0000u,0x0c8000u,0x0d0000u,0x0d8000u,0x0e0000u,0x0e8000u,0x0f0000u,0x0f8000u,
0x100000u,0x108000u,0x110000u,0x118000u,0x120000u,0x128000u,0x130000u,0x138000u,
0x140000u,0x148000u,0x150000u,0x158000u,0x160000u,0x168000u,0x170000u,0x178000u,
0x180000u,0x188000u,0x190000u,0x198000u,0x1a0000u,0x1a8000u,0x1b0000u,0x1b8000u,
0x1c0000u,0x1c8000u,0x1d0000u,0x1d8000u,0x1e0000u,0x1e8000u,0x1f0000u,0x1f8000u,
0x200000u,0x208000u,0x210000u,0x218000u,0x220000u,0x228000u,0x230000u,0x238000u,
0x240000u,0x248000u,0x250000u,0x258000u,0x260000u,0x268000u,0x270000u,0x278000u,
0x280000u,0x288000u,0x290000u,0x298000u,0x2a0000u,0x2a8000u,0x2b0000u,0x2b8000u,
0x2c0000u,0x2c8000u,0x2d0000u,0x2d8000u,0x2e0000u,0x2e8000u,0x2f0000u,0x2f8000u,
0x300000u,0x308000u,0x310000u,0x318000u,0x320000u,0x328000u,0x330000u,0x338000u,
0x340000u,0x348000u,0x350000u,0x358000u,0x360000u,0x368000u,0x370000u,0x378000u,
0x380000u,0x388000u,0x390000u,0x398000u,0x3a0000u,0x3a8000u,0x3b0000u,0x3b8000u,
0x3c0000u,0x3c8000u,0x3d0000u,0x3d8000u,0x3e0000u,0x3e8000u,0x3f0000u,0x3f8000u,
};
static uint32_t b2_dec_raw(unsigned i){double z=0.0;
if(i>=0u && i<64u){const double x=2.0*(double)(i-0u)/63.0-1.0;static const double c[6]={16.78093190581632,-1.0600058674014685,-0.0026504425629911652,0.00065917468110925569,-0.00012663230485992892,1.3746159604983523e-05};z=c[5];for(unsigned j=5;j>0;--j)z=z*x+c[j-1];}
if(i>=64u && i<128u){const double x=2.0*(double)(i-64u)/63.0-1.0;static const double c[6]={14.620229154757547,-1.0656222131280426,-0.00062418492690219829,0.00023319630475893309,-5.5996705234535394e-06,-6.9052255978751951e-05};z=c[5];for(unsigned j=5;j>0;--j)z=z*x+c[j-1];}
if(i>=128u && i<192u){const double x=2.0*(double)(i-128u)/63.0-1.0;static const double c[6]={12.453623198411121,-1.0667186753198852,5.0975818505659698e-06,-0.00027901971170106953,-0.00012431134446971409,0.00021841195766060883};z=c[5];for(unsigned j=5;j>0;--j)z=z*x+c[j-1];}
if(i>=192u && i<256u){const double x=2.0*(double)(i-192u)/63.0-1.0;static const double c[6]={10.285779751056726,-1.0664724103684011,-0.00075190385217483336,-0.0029669031317467302,0.00066502067338076998,0.0024410746087482828};z=c[5];for(unsigned j=5;j>0;--j)z=z*x+c[j-1];}
if(i>=256u && i<320u){const double x=2.0*(double)(i-256u)/63.0-1.0;static const double c[6]={8.1175434964886808,-1.067441793244561,0.00023566954720235226,0.0042427641907675619,0.00085301012217691991,-0.0025975825110592524};z=c[5];for(unsigned j=5;j>0;--j)z=z*x+c[j-1];}
if(i>=320u && i<384u){const double x=2.0*(double)(i-320u)/63.0-1.0;static const double c[6]={5.9498909457503153,-1.07252378799535,-0.0012040969622254005,0.023248222932743378,-0.0040936225161044438,-0.023120223828328061};z=c[5];for(unsigned j=5;j>0;--j)z=z*x+c[j-1];}
return (uint32_t)(8388608.0-b2_local_exp2(z)+0.5)&MD_MASK24;}
static uint32_t b2_hold_raw(unsigned i){double z=0.0;
if(i>=0u && i<16u){const double x=2.0*(double)(i-0u)/15.0-1.0;static const double c[6]={22.116440732394135,-0.22354937112690809,-0.011570093092800007,0.00060822963506890096,-2.4823551741806429e-06,-3.1624196189862257e-06};z=c[5];for(unsigned j=5;j>0;--j)z=z*x+c[j-1];}
if(i>=16u && i<32u){const double x=2.0*(double)(i-16u)/15.0-1.0;static const double c[6]={21.592629853569484,-0.26493294575141135,-0.0079389914471441874,0.00051004469406225523,-1.6210999987084402e-05,-2.0623237287843625e-06};z=c[5];for(unsigned j=5;j>0;--j)z=z*x+c[j-1];}
if(i>=32u && i<48u){const double x=2.0*(double)(i-32u)/15.0-1.0;static const double c[6]={20.995885586148447,-0.29253991575199767,-0.0051596657581955974,0.00036226162765241681,-1.5839407410084637e-05,1.4898183072193912e-07};z=c[5];for(unsigned j=5;j>0;--j)z=z*x+c[j-1];}
if(i>=48u && i<64u){const double x=2.0*(double)(i-48u)/15.0-1.0;static const double c[6]={20.351507892114427,-0.31021211808747806,-0.0032563791393440925,0.00023384182763336583,-1.1150075209655617e-05,4.5215421422157109e-06};z=c[5];for(unsigned j=5;j>0;--j)z=z*x+c[j-1];}
if(i>=64u && i<80u){const double x=2.0*(double)(i-64u)/15.0-1.0;static const double c[6]={19.676988729874353,-0.32127135965305348,-0.0020199083390307622,0.0001494586268255622,-9.2933173959288234e-06,2.5613479084189307e-06};z=c[5];for(unsigned j=5;j>0;--j)z=z*x+c[j-1];}
if(i>=80u && i<96u){const double x=2.0*(double)(i-80u)/15.0-1.0;static const double c[6]={18.983734939081113,-0.32810050059132284,-0.0012430270180262674,8.9020387603819983e-05,-4.1949290706227217e-06,6.4997703986487887e-06};z=c[5];for(unsigned j=5;j>0;--j)z=z*x+c[j-1];}
if(i>=96u && i<112u){const double x=2.0*(double)(i-96u)/15.0-1.0;static const double c[6]={18.278954528787722,-0.33228980028651145,-0.00075409576568579773,6.4649217717600686e-05,-6.5992175295939054e-06,-4.1908363238387883e-06};z=c[5];for(unsigned j=5;j>0;--j)z=z*x+c[j-1];}
if(i>=112u && i<128u){const double x=2.0*(double)(i-112u)/15.0-1.0;static const double c[6]={17.567129413291966,-0.33484472134556431,-0.00045758633243514544,5.5586212611604347e-05,-7.0044848412449213e-06,-1.5378093159773984e-05};z=c[5];for(unsigned j=5;j>0;--j)z=z*x+c[j-1];}
if(i>=128u && i<144u){const double x=2.0*(double)(i-128u)/15.0-1.0;static const double c[6]={16.851013676662802,-0.33637868775918034,-0.00028127144889011885,-1.5710124171608516e-05,-3.5271316802294898e-06,2.3633718483554125e-05};z=c[5];for(unsigned j=5;j>0;--j)z=z*x+c[j-1];}
if(i>=144u && i<160u){const double x=2.0*(double)(i-144u)/15.0-1.0;static const double c[6]={16.132287254317276,-0.33733534170046603,-0.00015829342129250619,2.4466552731508816e-05,-1.3051102974509625e-05,-8.9196400716787092e-06};z=c[5];for(unsigned j=5;j>0;--j)z=z*x+c[j-1];}
if(i>=160u && i<176u){const double x=2.0*(double)(i-160u)/15.0-1.0;static const double c[6]={15.411985650421176,-0.33787981025738373,-0.00012079648604032715,-7.3165249438570298e-05,5.344886176903913e-06,6.038335110310428e-05};z=c[5];for(unsigned j=5;j>0;--j)z=z*x+c[j-1];}
if(i>=176u && i<192u){const double x=2.0*(double)(i-176u)/15.0-1.0;static const double c[6]={14.690726978140916,-0.33825242794160487,-8.2783616354941623e-05,-4.8643322669117975e-05,3.5468072302808726e-05,7.7814482363945577e-05};z=c[5];for(unsigned j=5;j>0;--j)z=z*x+c[j-1];}
if(i>=192u && i<208u){const double x=2.0*(double)(i-192u)/15.0-1.0;static const double c[6]={13.968913445879046,-0.33849603142952889,-0.00011758979371559363,0.00018003751924383881,5.7251215571621437e-05,-0.00016425505292421523};z=c[5];for(unsigned j=5;j>0;--j)z=z*x+c[j-1];}
if(i>=208u && i<224u){const double x=2.0*(double)(i-208u)/15.0-1.0;static const double c[6]={13.246739961899552,-0.33854832787981481,-6.5459529390295704e-05,-0.00013508253325140713,6.8215984246171337e-05,0.00013329147793394606};z=c[5];for(unsigned j=5;j>0;--j)z=z*x+c[j-1];}
if(i>=224u && i<240u){const double x=2.0*(double)(i-224u)/15.0-1.0;static const double c[6]={12.524387959016893,-0.33870057537407144,1.3799714726762428e-05,0.00054703614359465568,-1.615475056851944e-06,-0.00046748385815942697};z=c[5];for(unsigned j=5;j>0;--j)z=z*x+c[j-1];}
if(i>=240u && i<256u){const double x=2.0*(double)(i-240u)/15.0-1.0;static const double c[6]={11.801951810628372,-0.33841605434605271,1.4585383768391315e-05,-0.00086068115472493411,-7.1762966005914041e-06,0.00076376162156579228};z=c[5];for(unsigned j=5;j>0;--j)z=z*x+c[j-1];}
return (uint32_t)(8388608.0-b2_local_exp2(z)+0.5)&MD_MASK24;}



static void b2_controls(mdkit_trx_b2 *s)
{
    const float *k = s->clock.current;
    const unsigned dist = (unsigned)k[MDKIT_TRX_B2_DIST];
    const unsigned dirt = (unsigned)k[MDKIT_TRX_B2_DIRT];
    s->pitch = b2_trx_b2_PTCH_cc128[(unsigned)k[MDKIT_TRX_B2_PTCH]];
    s->ramp_amount = b2_trx_b2_RAMP_cc128[(unsigned)k[MDKIT_TRX_B2_RAMP]];
    s->decay = b2_trx_b2_DEC_cc128[(unsigned)k[MDKIT_TRX_B2_DEC]];
    s->hold_decay = b2_trx_b2_HOLD_cc128[(unsigned)k[MDKIT_TRX_B2_HOLD]];
    s->warp[0] = b2_trx_b2_DIST0_cc128[dist];
    s->warp[1] = b2_trx_b2_DIST1_cc128[dist];
    s->warp[2] = b2_trx_b2_DIST2_cc128[dist];
    s->warp[3] = b2_trx_b2_DIST3_cc128[dist];
    s->dirt_shift = b2_trx_b2_DIRT_SHIFT_cc128[dirt];
    s->dirt_bias = b2_trx_b2_DIRT_BIAS_cc128[dirt];
    s->noise_level = b2_trx_b2_NOIS_cc128[(unsigned)k[MDKIT_TRX_B2_NOIS]];
    s->tick_level = b2_trx_b2_TICK_cc128[(unsigned)k[MDKIT_TRX_B2_TICK]];


    uint32_t raw[8];
    for (unsigned i = 0; i < 8; ++i) raw[i] = (uint32_t)(128.0f * k[i]);
    if (raw[MDKIT_TRX_B2_RAMP] & 127)
        s->ramp_amount = (76087u * raw[MDKIT_TRX_B2_RAMP]) >> 14;
    if (raw[MDKIT_TRX_B2_DEC] & 127)
        s->decay = b2_dec_raw((3 * raw[MDKIT_TRX_B2_DEC]) >> 7);
    if (raw[MDKIT_TRX_B2_HOLD] & 127)
        s->hold_decay = b2_hold_raw(raw[MDKIT_TRX_B2_HOLD] >> 6);
    if (raw[MDKIT_TRX_B2_DIST] & 127) {
        uint32_t amount = raw[MDKIT_TRX_B2_DIST] * raw[MDKIT_TRX_B2_DIST] >> 12;
        s->warp[0] = (0u - 0x8000u - 60*amount) & MD_MASK24;
        s->warp[1] = (0x8000u + 70*amount) & MD_MASK24;
        s->warp[2] = (0u - 0x8000u - 50*amount) & MD_MASK24;
        s->warp[3] = (0x8000u + 40*amount) & MD_MASK24;
    }
    if (raw[MDKIT_TRX_B2_NOIS] & 127) s->noise_level = raw[MDKIT_TRX_B2_NOIS] << 9;
    if (raw[MDKIT_TRX_B2_TICK] & 127) s->tick_level = raw[MDKIT_TRX_B2_TICK] << 8;
}

static void b2_init(void *state)
{
    mdkit_trx_b2 *s = state;

    b2_control_clock_seed(&s->clock, 8, mdkit_trx_b2_defaults);
    s->clock.next_update = 2;
    s->pos = 0;
    s->pending_trigger = s->triggered = s->target_mask = 0;
    for (unsigned i = 0; i < 8; ++i) {
        s->queued_targets[i] = mdkit_trx_b2_defaults[i];
        s->last_raw[i] = (unsigned)mdkit_trx_b2_defaults[i] * 128u;
    }
    s->ramp = s->phase = s->attack_step = s->amplitude = 0;
    s->noise_amplitude = s->tick = 0;
    for (unsigned i = 0; i < 32; ++i) s->block[i] = 0;
    s->noise_a = 0x12e70c;
    s->noise_b = 0x1914a5;
    s->hold = 0x7fffff;
    b2_controls(s);
}

static void b2_set_control(void *state, unsigned index, unsigned value)
{
    mdkit_trx_b2 *s = state;
    if (index >= 8) return;
    s->queued_targets[index] = value > 127 ? 127 : value;
    s->target_mask |= 1u << index;
}

static void b2_trigger(void *state)
{
    mdkit_trx_b2 *s = state;
    s->pending_trigger = 1;
}


static void b2_boundary(mdkit_trx_b2 *s)
{
    mdkit_control_clock *c = &s->clock;
    if (c->pending_valid && c->block >= c->publish_block) {
        for (unsigned i = 0; i < 8; ++i) c->current[i] = c->pending[i];
        c->pending_valid = 0;
    }
    if (c->block == c->next_update) {
        int changed = 0;
        for (unsigned i = 0; i < 8; ++i) {
            c->pending[i] = c->raw[i];
            uint32_t raw = (uint32_t)(128.0f * c->raw[i]);
            uint32_t target = (uint32_t)c->target[i];
            changed |= raw != s->last_raw[i];
            s->last_raw[i] = raw;
            c->raw[i] = (float)((3*raw + 128*target)/4) / 128.0f;
        }
        c->pending_valid = changed;
        c->publish_block = c->block + 1;
        c->next_update += 12;
    }
    for (unsigned i = 0; i < 8; ++i)
        if (s->target_mask & (1u << i)) c->target[i] = (float)s->queued_targets[i];
    s->target_mask = 0;
    if (!s->pending_trigger) {
        ++c->block;
        b2_controls(s);
        return;
    }
    for (unsigned i = 0; i < 8; ++i) c->current[i] = c->raw[i];
    b2_controls(s);
    if (!s->triggered) {

        s->noise_a = 0x12e70c;
        s->noise_b = 0x1914a5;
        s->phase = s->amplitude = 0;
    }
    s->ramp = s->ramp_amount;
    s->attack_step = 0x147ae;
    s->noise_amplitude = s->noise_level;
    s->tick = s->tick_level;
    s->hold = 0x7fffff;
    s->pending_trigger = 0;
    s->triggered = 1;
    ++c->block;

}

static void b2_block(mdkit_trx_b2 *s)
{
    uint32_t phase[32], tone[32], noise[32];


    uint64_t running_phase = b2_from24(s->phase);
    for (unsigned i = 0; i < 32; ++i) {
        s->ramp = b2_acc_to_word(b2_mpy(0x7fef9e, s->ramp));
        running_phase = b2_add(running_phase, b2_from24(s->pitch));
        running_phase = b2_add(running_phase, b2_from24(s->ramp));
        running_phase = b2_and_mid(running_phase, 0x7fffff);
        phase[i] = b2_mid(running_phase);
    }
    s->phase = b2_mid(running_phase);


    for (unsigned i = 0; i < 32; ++i) {
        const unsigned quadrant = phase[i] >> 21;
        uint64_t offset = b2_from24(phase[i]);
        if (quadrant) offset = b2_sub(offset, b2_from24(0x400000));
        if (quadrant == 3) offset = b2_sub(offset, b2_from24(0x400000));
        uint64_t warped = b2_mpy(s->warp[quadrant], b2_mid(offset));
        const uint64_t minimum = b2_from24(0xffe001);
        if (b2_s56(warped) < b2_s56(minimum)) warped = minimum;
        if (quadrant == 0 || quadrant == 2) warped = b2_sub(0, warped);
        if (quadrant == 1 || quadrant == 2)
            warped = b2_add(warped, b2_from24(0x4000));
        if (quadrant == 3) warped = b2_add(warped, b2_from24(0x8000));
        tone[i] = b2_local_sine((uint16_t)(b2_mid(warped) & 0x7fff));
    }


    for (unsigned i = 0; i < 32; ++i) {
        uint64_t quantized = b2_add(b2_from24(tone[i]), b2_from24(s->dirt_bias));
        quantized = b2_asr(quantized, s->dirt_shift);
        quantized &= ~(uint64_t)MD_MASK24;
        tone[i] = b2_acc_to_word(b2_asl(quantized, s->dirt_shift));
    }


    uint32_t first = s->noise_a, second = s->noise_b;
    for (unsigned i = 0; i < 8; ++i) {
        const uint32_t sum = (first + second) & MD_MASK24;
        noise[4*i] = sum;
        noise[4*i+1] = noise[4*i+2] = noise[4*i+3] = 0;
        if (i < 7) { first = second; second = sum; }
        else { s->noise_a = second; s->noise_b = sum; }
    }
    s->noise_amplitude = b2_acc_to_word(b2_mpy(s->noise_amplitude, 0x75c28f));
    const uint64_t hold = b2_mpy(s->hold_decay, s->hold);
    s->hold = b2_acc_to_word(hold);
    const uint32_t amplitude_decay = b2_s56(hold) >= b2_s56(b2_from24(0x147ae))
        ? 0x7ffff8 : s->decay;


    for (unsigned i = 0; i < 32; ++i) {
        uint64_t amplitude = b2_add(b2_mpy(s->amplitude, amplitude_decay),
                                    b2_from24(s->attack_step));
        if (b2_s56(amplitude) >= b2_s56(b2_from24(0x400000))) s->attack_step = 0;
        s->amplitude = b2_acc_to_word(amplitude);
        tone[i] = b2_acc_to_word(b2_mpy(tone[i], s->amplitude));
    }


    for (unsigned i = 0; i < 32; ++i) {
        uint64_t tick = b2_mpy(s->tick, 0x733333);
        s->tick = b2_acc_to_word(tick);
        uint64_t mixed = b2_add(b2_from24(tone[i]), b2_asl(tick, 3));
        mixed = b2_mac(mixed, noise[i], s->noise_amplitude);
        s->block[i] = b2_acc_to_word(mixed);
    }
}

static void b2_render(void *state, float *out, unsigned frames)
{
    mdkit_trx_b2 *s = state;
    while (frames) {
        if (!s->pos) {
            b2_boundary(s);
            b2_block(s);
        }
        unsigned count = 32 - s->pos;
        if (count > frames) count = frames;
        for (unsigned i = 0; i < count; ++i)
            out[i] = (float)b2_s24(s->block[s->pos+i]) / 8388608.0f;
        out += count;
        frames -= count;
        s->pos = (s->pos + count) & 31;
    }
}

static const char *const b2_names[8] = {
    "PTCH", "DEC", "RAMP", "HOLD", "TICK", "NOIS", "DIRT", "DIST"
};
const mdkit_voice mdkit_voice_trx_b2 = {
    "TRX-B2", 28, 8, b2_names, sizeof(mdkit_trx_b2),
    b2_init, b2_set_control, b2_trigger, b2_render, mdkit_trx_b2_defaults
};
