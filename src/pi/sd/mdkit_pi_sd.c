/* SPDX-License-Identifier: CC0-1.0
 * PI-SD standalone synthesis voice.
 *
 * Oscillator sources use eight-sample polynomial segments. Damping and
 * envelope coefficients use an inverse-time exponential model. Metallic waveshape and
 * coefficient banks use documented closed-form constructions.
 */
#include "mdkit_pi_sd.h"
#include <stdint.h>

/* ---- Default controls --------------------------------------------------- */
/* Active controls are in descriptor order; unused clock slots stay at 64. */
static const unsigned char mdkit_pi_sd_defaults[MDKIT_MAX_CONTROLS] = {
    64, 64, 10, 40, 90, 80, 80, 64
};


#define MD_MASK24 UINT32_C(0xffffff)
#define MD_MASK48 UINT64_C(0xffffffffffff)
#define MD_MASK56 UINT64_C(0xffffffffffffff)

static inline __attribute__((unused)) int64_t mdkit_signed_bits(uint64_t word, unsigned bits)
{
    const uint64_t sign = UINT64_C(1) << (bits - 1);
    const uint64_t mask = (UINT64_C(1) << bits) - 1;
    word &= mask;
    return (word & sign) ? (int64_t)word - (int64_t)(UINT64_C(1) << bits)
                         : (int64_t)word;
}
static inline __attribute__((unused)) int64_t mdkit_s24(uint32_t w) { return mdkit_signed_bits(w & MD_MASK24, 24); }
static inline __attribute__((unused)) int64_t mdkit_s56(uint64_t a) { return mdkit_signed_bits(a & MD_MASK56, 56); }
static inline __attribute__((unused)) uint32_t mdkit_mid(uint64_t a) { return (uint32_t)((a >> 24) & MD_MASK24); }
static inline __attribute__((unused)) uint32_t mdkit_low(uint64_t a) { return (uint32_t)(a & MD_MASK24); }
static inline __attribute__((unused)) uint64_t mdkit_from24(uint32_t w)
{ return ((uint64_t)(mdkit_s24(w) * (INT64_C(1) << 24))) & MD_MASK56; }
static inline __attribute__((unused)) uint64_t mdkit_from48(uint64_t w)
{ return (uint64_t)mdkit_signed_bits(w & MD_MASK48, 48) & MD_MASK56; }
static inline __attribute__((unused)) uint64_t mdkit_to48(uint64_t a)
{
    int64_t v = mdkit_s56(a);
    if(v < -INT64_C(0x800000000000)) v = -INT64_C(0x800000000000);
    if(v >  INT64_C(0x7fffffffffff)) v =  INT64_C(0x7fffffffffff);
    return (uint64_t)v & MD_MASK48;
}
static inline __attribute__((unused)) uint64_t mdkit_setmid(uint64_t a, uint32_t w)
{ return (a & ~(UINT64_C(0xffffff) << 24)) | ((uint64_t)(w & MD_MASK24) << 24); }
static inline __attribute__((unused)) uint64_t mdkit_add(uint64_t a, uint64_t b) { return (a + b) & MD_MASK56; }
static inline __attribute__((unused)) uint64_t mdkit_sub(uint64_t a, uint64_t b) { return (a - b) & MD_MASK56; }
static inline __attribute__((unused)) uint64_t mdkit_asl(uint64_t a, unsigned n) { return (a << n) & MD_MASK56; }
static inline __attribute__((unused)) uint64_t mdkit_asr(uint64_t a, unsigned n)
{
    const int64_t v = mdkit_s56(a);
    const int64_t sh = v >= 0 ? v / (INT64_C(1) << n)
        : -((-v + (INT64_C(1) << n) - 1) / (INT64_C(1) << n));
    return (uint64_t)sh & MD_MASK56;
}
static inline __attribute__((unused)) uint64_t mdkit_mpy(uint32_t a, uint32_t b)
{ return (uint64_t)(mdkit_s24(a) * mdkit_s24(b) * 2) & MD_MASK56; }
/* Mixed product with explicit numeric roles. For raw HH opcode 0x01278e,
 * QQQQ=14 decodes Y0 as signed and X1 as unsigned in the emulator. Do not
 * infer roles solely from the assembler's printed operand order. */
static inline __attribute__((unused)) uint64_t mdkit_mpy_su(uint32_t signed_word, uint32_t unsigned_word)
{
    return (uint64_t)(mdkit_s24(signed_word) *
                     (int64_t)(unsigned_word & MD_MASK24) * 2) & MD_MASK56;
}
static inline __attribute__((unused)) uint64_t mdkit_mpyr(uint32_t a, uint32_t b)
{
    uint64_t acc = mdkit_mpy(a, b);
    acc = (acc + (UINT64_C(1) << 23)) & MD_MASK56;
    if((acc & MD_MASK24) == 0) acc &= ~(UINT64_C(1) << 24);
    return acc & ~UINT64_C(0xffffff);
}
/* Add the product into the accumulator without rounding. */
static inline __attribute__((unused)) uint64_t mdkit_mac(uint64_t acc, uint32_t x, uint32_t y)
{
    return (acc + mdkit_mpy(x, y)) & MD_MASK56;
}
/* Add the product, then round the whole 56-bit sum.
 *
 * This is NOT A + mdkit_mpyr(x,y): rounding the sum and rounding the product
 * differ in the low bits, and EFM-XT only matched its oracle once the rounding
 * was applied to the sum. The tie handling copies mdkit_mpyr verbatim, and the
 * final mask is 64-bit on purpose: `~MD_MASK24` would be a 32-bit complement
 * and truncate the accumulator. */
static inline __attribute__((unused)) uint64_t mdkit_macr(uint64_t acc, uint32_t x, uint32_t y)
{
    uint64_t r = (acc + mdkit_mpy(x, y)) & MD_MASK56;
    r = (r + (UINT64_C(1) << 23)) & MD_MASK56;
    if((r & MD_MASK24) == 0) r &= ~(UINT64_C(1) << 24);
    return r & ~UINT64_C(0xffffff);
}
static inline __attribute__((unused)) uint64_t mdkit_and_mid(uint64_t a, uint32_t mask)
{ return mdkit_setmid(a, mdkit_mid(a) & mask); }
static inline __attribute__((unused)) uint64_t mdkit_pack48(uint32_t hi, uint32_t lo)
{ return ((uint64_t)(hi & MD_MASK24) << 24) | (lo & MD_MASK24); }
static inline __attribute__((unused)) uint32_t mdkit_acc_to_word(uint64_t a)
{
    const int64_t v = mdkit_s56(a);
    if(v < -(INT64_C(0x800000) << 24)) return UINT32_C(0x800000);
    if(v >  (INT64_C(0x7fffff) << 24)) return UINT32_C(0x7fffff);
    return mdkit_mid(a);
}


static __attribute__((unused)) double mdkit_sin_turn(unsigned i, double period)
{
  const double pi=3.14159265358979323846;
  double x=(double)i*2.0*pi/period, term=x, sum=x;
  for(unsigned k=1;k<=12;++k){term*=-x*x/((2.0*k)*(2.0*k+1.0));sum+=term;}
  return sum;
}
static __attribute__((unused)) double mdkit_cheb7(const double c[7], double row)
{
  double x=(row-63.5)/63.5,b1=0.0,b2=0.0;
  for(unsigned k=6;k>0;--k){double b=2.0*x*b1-b2+c[k];b2=b1;b1=b;}
  return x*b1-b2+c[0];
}
static __attribute__((unused)) uint32_t mdkit_word(double v)
{
  if(v>8388607.0)v=8388607.0; if(v<-8388608.0)v=-8388608.0;
  return (uint32_t)(int32_t)(v+(v<0.0?-0.5:0.5)) & 0xffffffu;
}


/* Inverse-time radius; the envelope is the 128th power of that radius. */
static uint32_t mdkit_decay_radius(unsigned index, unsigned power)
{
    double x = -0.020013062717699877 * (double)power / ((double)index + 0.06388299743157598);
    unsigned squares = 0;
    while (x < -0.5) { x *= 0.5; ++squares; }
    double term = 1.0, value = 1.0;
    for (unsigned k = 1; k <= 16; ++k) { term *= x / (double)k; value += term; }
    while (squares--) value *= value;
    return (uint32_t)(value * 8388608.0 + 0.5);
}

static uint32_t sd_damp(unsigned index)
{ return mdkit_decay_radius(index, 1u); }


static uint32_t sd_env(unsigned index)
{ return mdkit_decay_radius(index, 128u); }

static const uint32_t sd_boot_pts[40] = {
  -64684,-60117,-55696,-51429,-47330,-43399,-39645,-36069,-32671,-29456,-26424,-23582,
  -20923,-18448,-16152,-14023,-12084,-10315,-8720,-7305,-6017,-4903,-3939,-3118,
  -2538,-2055,-1726,-1554,-178961,-178463,-177580,-176183,-174297,-171981,-169241,-166119,
  -162653,-158867,-154801,-150485,
};

static const double sd_coef[142][8] = {
  {-0.034748962415970815, -0.055426714134349173, -0.021038418668629461, 0.0040991197535428696, 0.010524638555135492, 0.0016628527784632507, -0.0051677937406768033, -0.0019757689567389651},
  {-0.1449940076358163, -0.02234607089242974, 0.0030137178885018922, -0.0019915655696725646, 0.00087394184940625024, -0.00093232982504154688, 0.00062535271968551921, 0.0012949516005594229},
  {-0.17334305663017505, -0.0048323511355798862, 0.0025343765357521432, -0.00065492040387954691, -0.000775901434179909, -2.5490729240110783e-05, -0.00047489484697706933, 0.00018023272386926968},
  {-0.16655191775539094, 0.01736299968439806, 0.0039375872561826925, 0.00058477933544055641, -0.00068519649317343308, -0.00093734936751085093, 0.00012352687412606116, 9.3540398484037382e-05},
  {-0.11826316326049793, 0.024624452391253746, -0.0027280375984345419, -0.0028838955116346539, 0.00039163543850818754, 0.0017951669940076682, 0.0012071107962482348, -0.00077622670794678993},
  {-0.090204050435325686, 0.0013877084473279007, 0.0054245255122319681, -0.0024456789369651211, -0.0022411582848386831, 0.0013159995744394565, -0.00041831739860447958, 4.1961092352320846e-05},
  {-0.055601415370620091, 0.041651072564430555, -0.0063692940979914849, -0.018855290287622985, -0.0031014765838335381, 0.0028894252141981468, -0.00021230453288203905, -0.0012827082920921285},
  {-0.040024304367478968, 0.13011496099485764, 0.061423539713642672, -0.017574579059025087, -0.0021904217886103894, -0.0012220773687052638, -0.0014483579491854101, 0.0013412093832206034},
  {0.1603228660662554, 0.036320140964016902, 0.020205899639727749, -0.017201425625899019, -0.0069247839403235138, 0.005447239282309768, 0.00054404136284975425, -0.0010399417229362195},
  {0.23027554439645934, 0.033198399991185082, -0.029147406620096281, -0.024849327999011968, 0.00054593506469654727, 0.003738091355707086, 0.00062444915448494037, -0.00038569869486513756},
  {0.18501435931676591, 0.090228612117001214, 0.056466213637557106, -0.0081729274458363661, -0.0015928138585068927, -0.0001093018365685783, -0.00039725006261310251, -7.8776618667696035e-06},
  {0.46149948660483164, 0.057053454134741763, -0.027571742327684785, 0.002895703903959602, 0.0020925958609344418, 5.048585636457859e-05, -0.00023033779204221025, 5.0307181441467473e-05},
  {0.47496181002627247, -0.028314872175178647, -0.0060907107689771468, 0.0018660851534756493, 0.00078357135369309288, -0.00054288575051438968, -0.00060366715487257386, 6.5632122803226123e-05},
  {0.33940897630729722, -0.084129113347262321, -0.00070024650562520486, -0.0023531596558920291, 0.00072268115270856994, 0.0014998934358017143, -0.00051385752954566789, -0.00051112543371548136},
  {0.17398757325823277, -0.049223469129052948, 0.004548468747048522, -0.0013294390704080398, 0.0017768115667826311, 0.00056180321276363835, -0.0005948359939389311, -1.7907235592758968e-05},
  {0.12861797812680986, -0.013338266211758324, -0.014747419427173216, 0.00061295567896497592, 0.0020339574880684536, -0.00084535556013887723, -0.00028104206661132758, 0.0001661775293945931},
  {0.028415688126713374, -0.047724792089563074, 0.0066481147616691228, -0.0011118026986878305, -7.0384720472988055e-05, -0.00013565536866938553, -0.00046242089160335197, 0.00028824918798716405},
  {-0.020038927388264297, 0.0013832360061617989, 0.00086408209103359392, 0.0029284291998313474, -0.00048337826633037, -0.0025518478202987646, 0.00010074751985989962, 0.00057370445726216343},
  {-0.042216857438163259, -0.003723007458393109, 0.0019190969468682907, -0.0082018209777743305, 0.00107526122285182, 0.0013265159184407444, -0.00054852114400192828, -4.5706132328228445e-05},
  {-0.087381630034441382, -0.024577500454506657, -0.0071962341767680968, -0.0064909820033183943, 0.0027423378532047396, 0.0013813463608635622, -0.00097397195208435473, -6.3389854424007565e-05},
  {-0.11963738766474605, 0.011466883663585278, -0.011783833068693669, -0.0076731497553434265, 0.0011562196942880849, 0.00073086637613516115, 0.00035751221621467029, -0.0003180620839985892},
  {-0.17814266907888751, -0.020470304128078445, 0.0033355695044316892, -0.0069326684721696874, 0.0016008461746322487, 0.0011820256522898703, -0.00062276566984817785, 8.5919539358458058e-05},
  {-0.21085766278734405, -0.01368654926736778, -0.012910362732347743, -0.0019395829798061709, 0.0031404420809444957, 0.00053905137307690555, -0.00048991305173129516, -0.00027744682823443621},
  {-0.22535587712077859, 0.024535786973380439, -0.0011496748708143961, -0.0069041758045689924, -0.00011705352339452822, 0.00034908188495644564, -0.00010488014175079755, 8.2852173282947614e-05},
  {-0.23812020472364062, -0.0054914667313520375, 0.0097405093674346743, -0.0029123067585411634, -0.0014375988266288147, 9.7167227674324855e-05, 0.00031831175333036022, 0.00011263653565644555},
  {-0.27322484690529464, -0.030524069637188685, 0.0070678964017536691, 0.0058133846362883228, 0.00022610074091869136, -0.00049090391627638105, -0.00012318447047106372, 7.858877221831093e-05},
  {-0.22763028848292172, 0.034164682344271578, -0.018962190337010278, -0.0043794837511086275, 0.00079392731729773728, 0.00049519313348962983, 8.1026020456695751e-05, -0.00020889476315157264},
  {-0.27014097056631303, -0.017141887447542095, 0.021602927869915023, 0.002480668830382159, -0.001168402198931704, -0.0006220153031083267, -3.5586202082229119e-05, 0.00024522046201633024},
  {-0.23392324021454738, -0.0011143240042050051, -0.013165577093119983, 0.0022392778931437994, 0.00092518632203189609, -1.2327890322513272e-05, -8.6342788320737336e-05, -3.3603114247347331e-05},
  {-0.2012952523714274, 0.058048012415201834, 0.0087187709051956708, -0.0016344474379365967, 0.00049463604817801172, -0.00025542599317606519, -0.00013967691318411167, 4.2365318889399206e-05},
  {-0.069764340176900252, 0.049622041210144165, 0.0065487027768944099, 0.0017175519703111332, -0.0020405002318309418, -0.00026636830362904948, 0.00019015767425619337, 5.0751830632267987e-05},
  {0.035480140667156455, 0.039580896858751664, 0.0030997524675816135, -0.0032578790614934423, -0.0020301450446519433, 0.00082109279638745765, 0.00031572470349297663, -0.00016062060341128192},
  {0.097622431353617381, 0.019070248808869403, -0.003616506056399254, -0.0017545158216307774, 0.00093670750623763076, 8.5344450680070521e-05, -0.00021809686137813742, 5.1400970894772987e-05},
  {0.11073683223035509, 0.00019404547316903382, 0.0016307801534114639, -0.00048445261048377417, -0.00062495441339301561, 0.00011658810482104379, 0.00011433904271851628, -5.4168733772208698e-05},
  {0.10946289804406761, 0.00068324381700221321, 0.001077765003032661, -0.00040618205487893657, 0.00011673421839884065, 4.3779092710689036e-05, -4.4945235317290045e-05, 5.3655128289796376e-05},
  {0.10661386439913056, -0.011329430000235653, -0.0052049515607197538, 0.00067297458493460153, 0.00052982967010594588, 0.00010917785111814184, 8.5753093137322077e-05, -6.0263043269512141e-05},
  {0.083227720795871887, -0.0079178557225532218, -0.0034956194331041655, -0.0008714703344594527, 0.00015059859983416167, 9.4365496180313777e-05, 4.778433228948777e-05, -8.1525359120052289e-05},
  {0.057130440052747429, 0.0030010551932112615, 0.0088361231761357167, -0.00073173225003605276, -0.00031895211027909738, 1.0739517812098409e-05, -0.00017161080961360437, 5.1969265849845776e-05},
  {0.069909476955864275, -0.0061196321245006449, -0.0008343086204048401, 0.0014691002055163434, -0.00020679181462685461, 0.00014126630475301368, -1.5355852802828675e-05, -0.00014573793343717646},
  {0.070297010433649915, 0.0087644017762916783, 0.0031281335477817596, 6.6893599372023542e-05, -0.00017727328094123025, -1.3436286280923368e-05, 8.6219142556531845e-06, 5.9656703463891e-05},
  {0.073481215295032598, -0.023212467061239306, -0.0075532141612105206, 0.0021087023924337631, 0.00032699549046807957, -0.00011404215700976794, 2.1970901192008396e-05, -1.6169061483300222e-05},
  {0.034843331518075296, 0.0025783865491272656, 0.0044562994437405481, -0.00070127747888667385, -8.5985050261827467e-05, -7.9133628827954554e-05, -2.1148181298903382e-05, -2.3485561804491186e-05},
  {0.040067946666628516, -0.0025977654076389969, 0.00066989897131863082, 0.00022590665533546491, -0.00031287366897838253, 5.3142202179714919e-05, 4.4555275599109522e-05, 2.0242238295192549e-05},
  {0.036377483123861659, -0.0026383665124740238, -0.0014536604007136713, -0.00045226835148545047, 0.00016566125494944846, 0.00010192147524826754, -6.9013358895784102e-05, -2.7758474081768364e-05},
  {0.018877243240098724, -0.0091084472330597577, 0.0016757840920440185, 0.00043955112535816888, -0.00018108999469809632, -8.3426922275167705e-05, -1.9307761653546803e-06, 1.0348199349313851e-05},
  {0.0076344766599100288, -0.0027640363328828256, 0.00066159040418117376, 0.00028477048969458897, -8.3710317590482148e-05, -4.1695118084641579e-05, 5.0694763356460521e-05, 2.9627427364979957e-06},
  {0.0047152498261615007, -0.0020389673221264575, -0.00061308120736308658, 0.00012261831761861697, 4.4556731396973881e-05, -1.8834239138717854e-05, -1.2726002508618398e-05, 2.515240181916499e-05},
  {0.00038399672824501366, -0.00063187640820009001, 0.00040153817559864045, -0.0001165536717356494, -2.2439085781419453e-05, 4.8490879513944101e-05, -1.4580688972879573e-05, -3.3555558184161761e-05},
  {-7.6685065222490235e-06, 2.100220202313113e-06, -1.2235016361551566e-05, 1.5747573343105616e-06, 8.4630381833348021e-08, 2.7174893249289788e-09, 1.6302218460219198e-05, -2.1875789065638355e-06},
  {-4.2360350360266258e-07, -1.1490720135043386e-05, -6.783384176540394e-07, -1.0014433428295895e-05, -2.989238257414465e-08, -4.0361510198433847e-06, 1.1318343038308514e-06, 1.3501166338553769e-05},
  {6.3936762065471508e-06, -2.8839149308623374e-07, 1.0094947962594839e-05, 1.6440117178717888e-07, 4.1635818585442764e-07, 1.030268140311812e-06, -1.2911471155045253e-05, 1.0700114216888114e-07},
  {-1.479020364867109e-06, 8.4699967677200644e-06, -2.4848845189100887e-06, 7.6228695130945883e-06, -2.036175929889634e-07, 3.5921812013839278e-06, 2.6774063573813932e-06, -1.020790896291147e-05},
  {-5.0451617830226043e-06, -3.1774325179867561e-06, -7.8011235422713502e-06, -2.9357203554051676e-06, -3.4201544217971522e-07, -1.6658209561784137e-06, 1.0148463883928752e-05, 3.785462629619164e-06},
  {2.8435614593844469e-06, -6.4612610003678201e-06, 4.7437420612873644e-06, -5.6403847944845189e-06, 3.2415765518737584e-07, -2.4002224462391363e-06, -5.9445078982712963e-06, 7.5281248023707674e-06},
  {3.279324462507959e-06, 5.013174874572246e-06, 4.8632343451673625e-06, 4.3021946112276097e-06, 2.5175597531616387e-07, 1.6335507704449418e-06, -6.367756860628208e-06, -5.9421300951119886e-06},
  {-3.4463440695415378e-06, 4.4117630144076758e-06, -5.6592883791179447e-06, 3.7338095353334236e-06, -4.7886044032768872e-07, 1.246987913974509e-06, 7.0810978084207665e-06, -4.862607460785918e-06},
  {-1.7802923038187982e-06, -5.3061042611564902e-06, -2.6748809937594722e-06, -4.5048624694269825e-06, -2.0187063556569108e-08, -2.3105452985166392e-06, 3.4620813999532003e-06, 6.1014429067856588e-06},
  {3.8206128616972558e-06, -2.1275269318721266e-06, 5.7431106445922821e-06, -1.5948265475647844e-06, 4.0121788818699846e-07, -1.2568388127774507e-08, -7.4615463139101295e-06, 2.1852011034045287e-06},
  {1.5281321894791492e-07, 5.5680645649166259e-06, 1.9036686151392353e-07, 4.8436474268479964e-06, 1.8944782722326581e-07, 1.9688210159074444e-06, -5.3262790768510609e-07, -6.420068530133e-06},
  {-3.274429198225136e-06, -3.3643585387229624e-07, -5.4451256599147911e-06, -3.8089910958660952e-07, -2.6573163470149573e-07, -2.5646305503970051e-07, 6.4818914122750175e-06, 4.9696086029548148e-07},
  {9.2302135372948357e-07, -4.8841703877163485e-06, 1.5798159842638053e-06, -4.1336340170043247e-06, 3.9209488831054565e-08, -1.4484218101844972e-06, -1.5883725104180924e-06, 5.4594360537723585e-06},
  {2.7226709385609493e-06, 1.6722999134799516e-06, 4.5666932742784026e-06, 1.6015856090234614e-06, 1.6809612538458086e-07, 9.3549570010508857e-07, -5.430902415860654e-06, -2.1828233002452202e-06},
  {-1.7802923038187961e-06, 3.6338043476765392e-06, -2.6748809937594624e-06, 2.9032768604035207e-06, -2.0187063556583051e-08, 1.3750495984115516e-06, 3.4620813999532003e-06, -3.9186196065404386e-06},
  {-1.7975043899948822e-06, -1.8551709824047717e-06, -2.6630068330430545e-06, -2.1914956960245057e-06, -3.8627169689987139e-08, -1.5350417824164508e-06, 3.4858594315462857e-06, 2.5418715773008071e-06},
  {2.0846026826701658e-06, -2.3049698332518662e-06, 3.115773829954882e-06, -2.1917036671463101e-06, 3.6783159076650338e-07, -6.1992725224829146e-07, -4.0184873392313955e-06, 2.6132056720800603e-06},
  {8.3725915626726375e-07, 3.7725201157930682e-06, 1.3915961769978656e-06, 3.1763429433340245e-06, -4.6973744045123813e-08, 9.3821318943001285e-07, -1.645439786241493e-06, -4.3704022068090574e-06},
  {-1.9224296718473673e-06, 8.3614995673997442e-07, -3.1995960954292163e-06, 8.0079280451172995e-07, -2.9018903862581508e-07, 4.6774785005254751e-07, 4.3989358447207576e-06, -1.0914116501226105e-06},
  {-5.5653118990016877e-22, -2.4592645786469845e-06, -2.7653974549100868e-21, -2.3552729544462657e-06, 4.3491159137542946e-21, -1.3757289707427806e-06, -2.9194869429030883e-22, 3.2100342650665e-06},
  {1.897977199405433e-06, 9.5608593255747288e-07, 2.8764188755303626e-06, 3.8745019992347881e-07, 1.3975659385323342e-07, -7.9486562754029539e-08, -3.4240365494042635e-06, -7.2760776674840637e-07},
  {-4.045018512341712e-07, 2.2842360825355496e-06, -6.545425599647871e-07, 2.1389829877686392e-06, -6.2114041712581562e-09, 1.2344195258467337e-06, 1.0652558153702158e-06, -2.6774063573813915e-06},
  {-1.3764519988197026e-06, -3.7977812705018497e-07, -2.5687067843844298e-06, -8.3323629951337437e-07, -1.2597504084826199e-07, -7.5851920781941441e-07, 3.057854862870754e-06, 9.5825467320133381e-07},
  {1.1096468369942166e-06, -1.623114621907009e-06, 1.8191709386883287e-06, -1.5544801499345344e-06, 2.672844857443147e-07, -9.0798112069023746e-07, -2.1828233002452198e-06, 2.1186226149438903e-06},
  {1.2285340441546106e-06, 1.8610326151247172e-06, 1.7964486081230781e-06, 1.3614829488991161e-06, -7.7448445760339184e-08, 5.3840257250056801e-07, -2.4110924035388359e-06, -2.2708020171396359e-06},
  {-9.919619414075805e-07, 1.0546037932120578e-06, -1.6176330569174341e-06, 9.613465105455059e-07, -1.4771495544765614e-07, 5.4655504047534393e-07, 2.2208681507941553e-06, -1.0723892248481415e-06},
  {-4.5128545025363652e-07, -1.4753928553545876e-06, -6.6502034314908223e-07, -1.3582593965111315e-06, 3.4939148463330296e-09, -7.7652257459703587e-07, 1.1128118785563864e-06, 1.5836169040994735e-06},
  {1.3515201216149674e-06, -4.1536089459744547e-07, 1.6762630871704056e-06, -4.0387991854610528e-07, 5.5126212019887111e-08, -2.3778031593085256e-07, -2.6060722626021357e-06, 5.8018397087127815e-07},
  {3.6227595421859892e-22, 1.6722999134799484e-06, 1.8805977764705038e-21, 1.6015856090234605e-06, -3.4293296887588406e-21, 9.3549570010509132e-07, 2.8816735391537711e-22, -2.1828233002452198e-06},
  {-9.3046773043978646e-07, -2.184538364720851e-07, -1.5819630385117814e-06, -1.6055370603377686e-07, -1.4247408317815911e-07, -7.880719042279541e-08, 2.1780676939266023e-06, -1.9022425274467801e-08},
  {5.685644282493742e-07, -1.5978243633677899e-06, 6.0639031289610859e-07, -1.5594714568578651e-06, 8.9094828581440939e-08, -3.3730836245619202e-07, -7.2760776674840775e-07, 2.0044880632970816e-06},
  {8.2234255387447897e-07, -1.199359758174965e-07, 1.6993082681437954e-06, 4.1334260458825225e-07, -3.3192191040144109e-08, 5.4723441280657296e-07, -2.0116214727750062e-06, -3.6380388337420372e-07},
  {-8.2025306053563175e-07, 1.686004119013283e-06, -1.0705660113469788e-06, 1.1451929822214888e-06, 9.4141594470579947e-08, 3.9709312760452083e-07, 1.2602356744335153e-06, -1.7381741094545269e-06},
  {-6.7064546682457996e-07, -9.851786065458745e-08, -8.5571005507113633e-07, -5.7389631062202826e-07, 2.47097422187734e-07, -6.2604160322936991e-07, 1.2792580997079825e-06, 3.4478145809973531e-07},
  {5.221867468208079e-07, -3.9348233258351728e-07, 8.5608044173568436e-07, -3.768436727114025e-07, 1.2578093446791194e-07, -2.2011663531884545e-07, -1.0272109648212798e-06, 5.1360548241064001e-07},
  {1.5116529539227846e-08, 1.1551567392113327e-06, 2.8536014724522944e-07, 1.1355223250575374e-06, 4.1926978155967335e-08, 8.9677147722490828e-08, -3.4240365494042544e-07, -1.4266818955851112e-06},
  {-5.221867468208079e-07, 3.9348233258351728e-07, -8.5608044173568436e-07, 3.768436727114025e-07, -1.2578093446791194e-07, 2.2011663531884545e-07, 1.0272109648212798e-06, -5.1360548241064001e-07},
  {-1.9889750919482654e-22, -7.8696466516703435e-07, -9.3417310591143185e-22, -7.5368734542280489e-07, 1.2976303190037847e-21, -4.4023327063769005e-07, -4.8815001074021543e-23, 1.0272109648212798e-06},
  {4.0450185123417162e-07, 1.750284961114338e-07, 6.5454255996478996e-07, 2.1628996667762641e-07, 6.2114041712536655e-09, 1.4130944489604781e-07, -1.0652558153702156e-06, -5.326279076851082e-07},
  {1.9889750919482654e-22, 7.8696466516703435e-07, 9.3417310591143185e-22, 7.5368734542280489e-07, -1.2976303190037847e-21, 4.4023327063769005e-07, 4.8815001074021543e-23, -1.0272109648212798e-06},
  {-5.2218674682080801e-07, -3.9348233258351749e-07, -8.5608044173568532e-07, -3.7684367271140265e-07, -1.2578093446791051e-07, -2.2011663531884465e-07, 1.0272109648212796e-06, 5.1360548241064001e-07},
  {5.0707021728158027e-07, -7.6167440662781474e-07, 5.7072029449045582e-07, -7.586786523461345e-07, 8.3853956311943193e-08, 1.3043948759635348e-07, -6.8480730988085427e-07, 9.1307641317447098e-07},
  {5.0707021728158037e-07, 7.6167440662781485e-07, 5.7072029449045741e-07, 7.5867865234613482e-07, 8.3853956311938548e-08, -1.3043948759635425e-07, -6.8480730988085204e-07, -9.1307641317447119e-07},
  {-5.221867468208079e-07, 3.9348233258351728e-07, -8.5608044173568436e-07, 3.768436727114025e-07, -1.2578093446791194e-07, 2.2011663531884545e-07, 1.0272109648212798e-06, -5.1360548241064001e-07},
  {-4.9195368774235263e-07, -3.934823325835176e-07, -2.8536014724522876e-07, -3.7684367271140276e-07, -4.1926978155969724e-08, -2.2011663531884473e-07, 3.4240365494042639e-07, 5.1360548241064022e-07},
  {3.0233059078455314e-08, 3.4308755588520642e-22, 5.7072029449045656e-07, 1.9116302352929203e-22, 8.3853956311940811e-08, -3.4609561619756779e-22, -6.84807309880853e-07, -1.4374954134896624e-22},
  {0, 0, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, 0, 0},
};
static uint32_t sd_sample(unsigned index)
{
  unsigned segment=index/8u; if(segment>=142u){segment=141u;index=1131u;}
  const unsigned local=index%8u; const double x=(double)local*(2.0/7.0)-1.0;
  const double *c=sd_coef[segment]; double b1=0.0,b2=0.0;
  for(unsigned k=7u;k>0u;--k){const double b=2.0*x*b1-b2+c[k];b2=b1;b1=b;}
  return mdkit_word((x*b1-b2+c[0])*8388608.0);
}

static const uint32_t sd_a[8] = {
  0x7f07e9u,0x7f08a4u,0x7f095eu,0x7f0a17u,0x7f0acfu,0x7f0b85u,0x7f0c3bu,0x7f0cf0u,
};

static const uint32_t sd_b[8] = {
  0x7f0da4u,0x7f0e56u,0x7f0f08u,0x7f0fb8u,0x7f1068u,0x7f1116u,0x7f11c4u,0x7f1271u,
};

static const uint32_t sd_c[8] = {
  0x7f131cu,0x7f13c7u,0x7f1471u,0x7f1519u,0x7f15c1u,0x7f1668u,0x7f170eu,0x7f17b3u,
};

static const uint32_t sd_d[8] = {
  0x7f1857u,0x7f18fau,0x7f199du,0x7f1a3eu,0x7f1adfu,0x7f1b7eu,0x7f1c1du,0x7f1cbbu,
};

static const uint32_t sd_e[8] = {
  0x02b542u,0x046070u,0x05482fu,0x07be0fu,0x0c5f34u,0x0c9558u,0x12e4f8u,0x13d747u,
};

static const uint32_t sd_f[8] = {
  0x00000du,0x000022u,0x000025u,0x00001au,0x000015u,0x000100u,0x000100u,0x0001ffu,
};

static const uint32_t sd_g[8] = {
  0x800000u,0xae147bu,0xd70a3du,0xeb851fu,0xf5c28fu,0xfae148u,0xfef9dbu,0xffbe77u,
};

static uint32_t sd_boot_x(unsigned a)
{ if(a==0u)return 0xfffcdbu; if(a==1u)return 0x000323u; return 0u; }
static uint32_t sd_boot_y(unsigned a)
{
  static const uint32_t head[4]={0x405218u,0xbf1c44u,0u,0u};
  if(a<4u)return head[a]; if(a==320u)return 0x100u; if(a==321u)return 0x800u;
  if(a>=355u&&a<512u){unsigned q=(a-355u)/4u,f=(a-355u)%4u;uint32_t x=sd_boot_pts[q];
    if(a==511u)return x; uint32_t y=sd_boot_pts[q+1u]; return (uint32_t)((int64_t)x+((int64_t)y-(int64_t)x)*(int64_t)f/4)&0xffffffu;}
  return 0u;
}

static uint32_t sd_rom(uint32_t address)
{
  uint32_t a=address&0xffffffu;
  if (a>=0x142949u && a<0x142b49u) return sd_damp(a-0x142949u);
  if (a>=0x142b49u && a<0x142d49u) return sd_env(a-0x142b49u);
  if (a >= 0x142f4bu && a < 0x1433b7u) return sd_sample(a - 0x142f4bu);
  if (a >= 0x142c9au && a < 0x142ca2u) return sd_a[a - 0x142c9au];
  if (a >= 0x142ca2u && a < 0x142caau) return sd_b[a - 0x142ca2u];
  if (a >= 0x142caau && a < 0x142cb2u) return sd_c[a - 0x142caau];
  if (a >= 0x142cb2u && a < 0x142cbau) return sd_d[a - 0x142cb2u];
  if (a >= 0x142f33u && a < 0x142f3bu) return sd_e[a - 0x142f33u];
  if (a >= 0x142f3bu && a < 0x142f43u) return sd_f[a - 0x142f3bu];
  if (a >= 0x142f43u && a < 0x142f4bu) return sd_g[a - 0x142f43u];
  return 0u;
}


enum { MDKIT_PI_PUBLISH = 1, MDKIT_PI_TRIGGER = 2, MDKIT_PI_FIRST = 4 };
static inline void mdkit_events_init(mdkit_control_clock *c, mdkit_pi_sd_events *e) {

  for(unsigned i=0;i<8u;++i){const float v=(float)mdkit_pi_sd_defaults[i];c->target[i]=c->raw[i]=c->current[i]=c->pending[i]=v;}
  c->count = 8; c->block = 0; c->publish_block = 0; c->pending_valid = 0;
  c->next_update = 2;
  e->targets = e->trigger = e->triggered = 0;
  for (unsigned i = 0; i < 8; ++i) {
    e->queued[i] = mdkit_pi_sd_defaults[i];
    e->last_raw[i] = (unsigned)mdkit_pi_sd_defaults[i] * 128u;
  }
}
static inline void mdkit_events_target(mdkit_pi_sd_events *e, unsigned count,
                                       unsigned i, unsigned value) {
  if (i < count) {
    e->queued[i] = value > 127 ? 127 : value;
    e->targets |= 1u << i;
  }
}
static inline unsigned
mdkit_events_boundary(mdkit_control_clock *c, mdkit_pi_sd_events *e, int trigger_only) {
  unsigned result = 0;
  if (c->pending_valid && c->block >= c->publish_block) {
    for (unsigned i = 0; i < 8; ++i)
      c->current[i] = c->pending[i];
    c->pending_valid = 0;
    result = MDKIT_PI_PUBLISH;
  }
  if (c->block == c->next_update) {

    for (unsigned i = 0; i < 8; ++i) {
      uint32_t raw = (uint32_t)(128.0f * c->raw[i]);
      c->pending[i] = c->raw[i];
      e->last_raw[i] = raw;
      c->raw[i] =
          (float)((3 * raw + 128 * (uint32_t)c->target[i]) / 4) / 128.0f;
    }
    c->pending_valid = !trigger_only;
    c->publish_block = c->block + 1;
    c->next_update += 12;
  }
  for (unsigned i = 0; i < 8; ++i)
    if (e->targets & (1u << i))
      c->target[i] = (float)e->queued[i];
  e->targets = 0;
  if (e->trigger) {
    for (unsigned i = 0; i < 8; ++i)
      c->current[i] = c->raw[i];
    result = MDKIT_PI_PUBLISH | MDKIT_PI_TRIGGER | (e->triggered ? 0 : MDKIT_PI_FIRST);
    e->trigger = 0;
    e->triggered = 1;
  }
  ++c->block;
  return result;
}

static inline __attribute__((unused)) uint32_t mdkit_pi_sd_modal_x(mdkit_pi_sd_modal *c, uint32_t address) {
  if (address < 512u) return c->work[address];
  if (address >= 0x142000u && address < 0x145000u) return sd_rom(address);
  return sd_boot_x(address);
}
static inline __attribute__((unused)) uint32_t mdkit_pi_sd_modal_y(mdkit_pi_sd_modal *c, uint32_t address) {
  if (address < 512u) return c->fraction[address];
  return (address >= 0x142000u && address < 0x145000u) ? sd_rom(address) : 0u;
}
static inline __attribute__((unused)) void mdkit_pi_sd_modal_reset(mdkit_pi_sd_modal *c) {
  for (unsigned i = 0; i < 512; ++i) {
    c->work[i] = sd_boot_x(i);
    c->fraction[i] = sd_boot_y(i);
  }
  for (unsigned i = 0; i < 8; ++i)
    for (unsigned j = 0; j < 3; ++j)
      c->history[i][j] = 0;
}
static inline __attribute__((unused)) void mdkit_pi_sd_modal_clear_modes(mdkit_pi_sd_modal *c) {
  for (unsigned i = 0; i < 8; ++i)
    for (unsigned j = 0; j < 3; ++j)
      c->history[i][j] = 0;
}
static inline __attribute__((unused)) uint32_t mdkit_pi_sd_modal_wrap(uint32_t p, int step, unsigned m) {
  unsigned mask = 1;
  while (mask <= m)
    mask <<= 1;
  --mask;
  int64_t v = (int64_t)p + step;
  unsigned base = p & ~mask;
  int64_t span = ((unsigned)step & mask) ? m + 1 : 0;
  if (v < base)
    v += span;
  if (v > base + m)
    v -= span;
  return (uint32_t)v & MD_MASK24;
}
/* Integer/fraction phase identifies two adjacent waveform words; complementary
 * unsigned fractions are generated before either table read. */
static inline __attribute__((unused)) void mdkit_pi_sd_modal_excitation(mdkit_pi_sd_modal *c, uint32_t *hi,
                                       uint32_t *lo, uint64_t step,
                                       uint32_t end) {
  c->work[64] = 0x7fffff;
  c->fraction[64] = 0xffffff;
  uint64_t phase = mdkit_from48(mdkit_pack48(*hi, *lo));
  for (unsigned i = 0; i < 32; ++i) {
    phase = mdkit_add(phase, step);
    uint64_t complement = mdkit_sub(mdkit_from48(UINT64_C(0x7fffffffffff)), phase);
    uint64_t v = mdkit_to48(phase);
    c->work[i] = mdkit_mid(v);
    c->fraction[i] = mdkit_low(v);
    c->fraction[32 + i] = mdkit_low(complement);
  }
  if (mdkit_s56(phase) >= mdkit_s56(mdkit_from24(end)))
    phase = mdkit_from24(end);
  *hi = mdkit_mid(phase);
  *lo = mdkit_low(phase);
  uint32_t first = mdkit_pi_sd_modal_x(c, c->work[0]);
  for (unsigned i = 0; i < 32; ++i) {
    uint32_t index = c->work[i];
    uint64_t a = mdkit_mpy_su(first, c->fraction[32 + i]);
    a = mdkit_add(a, mdkit_mpy_su(mdkit_pi_sd_modal_x(c, index + 1), c->fraction[i]));
    first = mdkit_pi_sd_modal_x(c, c->work[i + 1]);
    c->work[64 + i] = mdkit_acc_to_word(a);
  }
}
/* The eight modes receive a cubic normalization polynomial and measured
 * damping-table taps. Their five MACs preserve the two parallel transfers. */
static inline __attribute__((unused)) void mdkit_pi_sd_modal_coefficients(mdkit_pi_sd_modal *c, uint32_t pitch,
                                         uint32_t decay, uint32_t freq_table,
                                         uint32_t amp_table) {
  uint32_t *y = c->fraction;
  for (unsigned i = 0; i < 8; ++i)
    y[0xb4 + i] = mdkit_acc_to_word(mdkit_mpy(mdkit_pi_sd_modal_y(c, freq_table + i), pitch));
  for (unsigned i = 0; i < 8; ++i) {
    uint32_t f = y[0xb4 + i];
    uint64_t b = mdkit_sub(0, mdkit_mpy(f, f));
    uint64_t a = mdkit_add(mdkit_from24(0x7fffff), b);
    uint32_t old = mdkit_acc_to_word(b);
    b = mdkit_mpy(old, 0x155555);
    a = mdkit_mac(a, mdkit_acc_to_word(b), old);
    y[0xa0 + i] = mdkit_acc_to_word(a);
  }
  for (unsigned i = 0; i < 8; ++i) {
    uint32_t idx =
        mdkit_acc_to_word(mdkit_mpy(mdkit_pi_sd_modal_x(c, amp_table - 8 + i), decay));
    uint32_t damping = mdkit_pi_sd_modal_x(c, 0x142949 + idx);
    uint64_t a =
        mdkit_add(mdkit_asl(mdkit_mpy(damping, y[0xa0 + i]), 1), mdkit_from24(0x800000));
    uint32_t first = mdkit_acc_to_word(a);
    uint64_t b = mdkit_sub(0, mdkit_mpy(damping, damping));
    a = mdkit_add(a, b);
    uint32_t normalized = mdkit_acc_to_word(a);
    uint64_t gain = mdkit_asl(mdkit_mpy(mdkit_pi_sd_modal_x(c, amp_table + i), normalized), 1);
    y[0x64 + 4 * i] = mdkit_acc_to_word(b);
    y[0x65 + 4 * i] = first;
    y[0x66 + 4 * i] = mdkit_acc_to_word(gain);
  }
}
static inline __attribute__((unused)) void mdkit_pi_sd_modal_filter(mdkit_pi_sd_modal *c) {
  uint32_t *w = c->work, *y = c->fraction;

  for (unsigned mode = 0; mode < 8; ++mode) {
    unsigned coef = 0x64 + mode * 4, read = 0x60, write = 0x40;
    uint64_t a = mdkit_from24(c->history[mode][0]);
    a = (a & ~UINT64_C(0xffffff)) | c->history[mode][1];
    uint32_t x1 = c->history[mode][2], x0 = w[read++];
    uint32_t y0 = y[coef];
    coef = mdkit_pi_sd_modal_wrap(coef, 1, 2);
    uint32_t old = mdkit_acc_to_word(a);
    a = mdkit_mac(a, x1, y0);
    x1 = old;
    y0 = y[coef];
    coef = mdkit_pi_sd_modal_wrap(coef, 1, 2);
    a = mdkit_mac(a, x1, y0);
    y0 = y[coef];
    coef = mdkit_pi_sd_modal_wrap(coef, 1, 2);
    uint64_t b = mdkit_from24(w[write]);
    a = mdkit_mac(a, y0, x0);
    y0 = y[coef];
    coef = mdkit_pi_sd_modal_wrap(coef, 1, 2);
    for (unsigned i = 0; i < 31; ++i) {
      b = mdkit_add(b, a);
      x0 = w[read++];
      old = mdkit_acc_to_word(a);
      a = mdkit_mac(a, x1, y0);
      x1 = old;
      y0 = y[coef];
      coef = mdkit_pi_sd_modal_wrap(coef, 1, 2);
      a = mdkit_mac(a, x1, y0);
      w[write++] = mdkit_acc_to_word(b);
      y0 = y[coef];
      coef = mdkit_pi_sd_modal_wrap(coef, 1, 2);
      b = mdkit_from24(w[write]);
      a = mdkit_mac(a, y0, x0);
      y0 = y[coef];
      coef = mdkit_pi_sd_modal_wrap(coef, 1, 2);
    }
    b = mdkit_add(b, a);
    w[write] = mdkit_acc_to_word(b);

    c->history[mode][0] = mdkit_acc_to_word(a);
    c->history[mode][1] = mdkit_low(a);
    c->history[mode][2] = x1;
  }
}

static __attribute__((unused)) uint32_t mdkit_drum_excitation_x(mdkit_pi_sd*s,uint32_t address)
{
 unsigned base=s->variant?0x829u:0x82au;
 if(s->variant&&address==0x811u)return s->phase_hi;
 if(address>=base&&address<base+16u){unsigned o=address-base;return s->body.history[o/2][o&1u?2:0];}
 return mdkit_pi_sd_modal_x(&s->body,address);
}
static __attribute__((unused)) void mdkit_drum_zero(mdkit_pi_sd*s){volatile unsigned char*p=(volatile unsigned char*)s;for(unsigned i=0;i<sizeof(*s);++i)p[i]=0;}
/* Idle excitation can traverse the voice's live X-state before its first
 * trigger. These addresses must observe current history, rather than boot ROM. */
static __attribute__((unused)) uint32_t drum_excitation_x(mdkit_pi_sd*s,uint32_t address){
 unsigned base=s->variant?0x829:0x82a;
 if(s->variant&&address==0x811)return s->phase_hi;
 if(address>=base&&address<base+16){unsigned offset=address-base;return s->body.history[offset/2][offset&1?2:0];}
 return mdkit_pi_sd_modal_x(&s->body,address);
}
/* Each modal lane retains the firmware's one-sample pipeline and the carry
 * from the preceding lane; output/scratch stores precede the delayed reads. */
static __attribute__((unused)) void drum_modes(mdkit_pi_sd*s,uint64_t carry){
 mdkit_pi_sd_modal*c=&s->body;uint32_t*w=c->work,*y=c->fraction;unsigned source=0x60,coef=0x64;
 uint32_t input=w[source++],coefficient=y[coef];coef=mdkit_pi_sd_modal_wrap(coef,1,2);
 for(unsigned mode=0;mode<8;++mode){
  uint64_t state=mdkit_from48(mdkit_pack48(c->history[mode][0],c->history[mode][1]));uint32_t previous=c->history[mode][2];unsigned output=0x3f,ring=31;
  for(unsigned i=0;i<32;++i){
   uint32_t old=mdkit_acc_to_word(state);state=mdkit_mac(state,previous,coefficient);previous=old;coefficient=y[coef];coef=mdkit_pi_sd_modal_wrap(coef,1,2);
   state=mdkit_mac(state,previous,coefficient);w[output++]=mdkit_acc_to_word(carry);coefficient=y[coef];coef=mdkit_pi_sd_modal_wrap(coef,1,2);
   uint64_t next=mdkit_from24(w[output]);s->block[ring]=mdkit_acc_to_word(carry);ring=(ring+1)&31;
   state=mdkit_mac(state,coefficient,input);carry=mdkit_add(next,state);input=w[source];source=0x60+((source+1)&31);coefficient=y[coef];coef=mdkit_pi_sd_modal_wrap(coef,1,2);
  }
  coef=mdkit_pi_sd_modal_wrap(coef,6,2);w[output]=mdkit_acc_to_word(carry);coefficient=y[coef];coef=mdkit_pi_sd_modal_wrap(coef,1,2);
  uint64_t stored=mdkit_to48(state);c->history[mode][0]=mdkit_mid(stored);c->history[mode][1]=mdkit_low(stored);c->history[mode][2]=previous;s->block[ring]=mdkit_acc_to_word(carry);
 }
}
static __attribute__((unused)) uint64_t drum_coefficients(mdkit_pi_sd*s,uint32_t pitch){
 mdkit_pi_sd_modal*c=&s->body;uint32_t*y=c->fraction,*w=c->work;uint32_t base=s->variant?0x142f33:0x144c9a;
 for(unsigned i=0;i<8;++i)y[0xb4+i]=mdkit_acc_to_word(mdkit_mpy(mdkit_pi_sd_modal_x(c,base+i),i<4?pitch:s->parameters[0]));
 for(unsigned i=0;i<8;++i){uint64_t square=mdkit_sub(0,mdkit_mpy(y[0xb4+i],y[0xb4+i]));uint32_t q=mdkit_acc_to_word(square);uint64_t poly=mdkit_add(mdkit_from24(0x7fffff),square);poly=mdkit_mac(poly,mdkit_acc_to_word(mdkit_mpy(0x155555,q)),q);y[0xa0+i]=mdkit_acc_to_word(poly);}
 for(unsigned i=0;i<8;++i)w[i]=i<4?mdkit_pi_sd_modal_y(c,base+16+i):mdkit_acc_to_word(mdkit_mpy(s->parameters[7],mdkit_pi_sd_modal_y(c,base+16+i)));
 uint64_t last=0;
 for(unsigned i=0;i<8;++i){uint32_t idx=mdkit_acc_to_word(mdkit_mpy(mdkit_pi_sd_modal_x(c,base+8+i),s->parameters[1]));uint32_t damping=mdkit_pi_sd_modal_x(c,0x142949+idx);
  uint64_t gain=mdkit_mpy(damping,y[0xa0+i]);
  gain=s->variant?mdkit_asl(mdkit_sub(gain,mdkit_from24(0x400000)),1):mdkit_add(mdkit_asl(gain,1),mdkit_from24(0x800000));
  uint32_t first=mdkit_acc_to_word(gain);uint64_t square=mdkit_sub(0,mdkit_mpy(damping,damping));gain=mdkit_add(gain,square);
  last=mdkit_mpy(mdkit_pi_sd_modal_x(c,base+9+i),s->parameters[1]);
  y[0x64+4*i]=mdkit_acc_to_word(square);y[0x65+4*i]=first;y[0x66+4*i]=mdkit_acc_to_word(mdkit_asl(mdkit_mpy(w[i],mdkit_acc_to_word(gain)),1));
 }
 return last;
}
static __attribute__((unused)) void drum_delays(mdkit_pi_sd*s,uint32_t excitation_gain,uint32_t ring_gain,uint32_t noise_gain){
 uint32_t*w=s->body.work,*y=s->body.fraction;
 uint64_t excitation=mdkit_from24(s->source_filter);
 for(unsigned lane=0;lane<2;++lane){
  unsigned cursor=(s->delay_position[lane]+(lane?255:267))&511;uint64_t delayed=mdkit_from24(s->delay_filter[lane]);
  for(unsigned i=0;i<32;++i){
   if(!lane){uint32_t old=mdkit_acc_to_word(excitation);w[0x60+i]=old;excitation=mdkit_mac(excitation,s->variant?y[0x40+i]:w[0x40+i],excitation_gain);excitation=mdkit_sub(excitation,mdkit_mpy(old,excitation_gain));}
   uint32_t old=mdkit_acc_to_word(delayed);w[0x80+32*lane+i]=old;delayed=mdkit_mac(delayed,s->delay_x[lane][cursor],ring_gain);delayed=mdkit_sub(delayed,mdkit_mpy(old,ring_gain));cursor=(cursor+1)&511;
  }s->delay_filter[lane]=mdkit_acc_to_word(delayed);
 }s->source_filter=mdkit_acc_to_word(excitation);
 uint64_t sum=mdkit_from24(s->noise_a);uint32_t next=s->noise_b;
 for(unsigned i=0;i<16;++i){uint32_t old=next;sum=mdkit_add(sum,mdkit_from24(next));y[2*i]=old;next=mdkit_mid(sum);y[2*i+1]=next;next=(old+next)&0xffffff;}
 s->noise_a=next;s->noise_b=mdkit_mid(sum);y[0xfa]=mdkit_mid(sum);
 unsigned cursor=(s->delay_position[2]+181)&511;uint64_t delayed=mdkit_from24(s->delay_filter[2]),noise=mdkit_from24(s->noise_filter);
 uint32_t residual=0;
 for(unsigned i=0;i<32;++i){uint32_t old=mdkit_acc_to_word(delayed);w[0xc0+i]=old;
  /* External X/Y accesses alias the same physical delay bank. */
  delayed=mdkit_mac(delayed,s->delay_y[cursor],noise_gain);delayed=mdkit_sub(delayed,mdkit_mpy(old,noise_gain));
  uint32_t n=y[i],before=mdkit_acc_to_word(noise);noise=mdkit_add(noise,mdkit_from24(n));y[i]=before;noise=mdkit_mac(noise,0xd9999a,before);residual=before;cursor=(cursor+1)&511;
 }s->delay_filter[2]=mdkit_acc_to_word(delayed);s->noise_filter=mdkit_acc_to_word(noise);
 uint64_t absnoise=mdkit_from24(y[0xfa]);if(mdkit_s56(absnoise)<0)absnoise=mdkit_sub(0,absnoise);
 if(mdkit_s56(absnoise)>=mdkit_s56(mdkit_from24(s->rattle_threshold))){residual=0x7fffff;s->rattle_gate=residual;}
 uint32_t rattle=mdkit_acc_to_word(mdkit_mpy(s->rattle_threshold,residual));rattle=mdkit_acc_to_word(mdkit_mpy(s->parameters[5],rattle));
 uint32_t small=mdkit_acc_to_word(mdkit_mpy(0x266666,rattle));
 if(!s->variant){for(unsigned i=0;i<32;++i){w[0x40+i]=mdkit_acc_to_word(mdkit_mac(mdkit_from24(w[0x40+i]),small,y[i]));w[0x60+i]=mdkit_acc_to_word(mdkit_mac(mdkit_from24(w[0x60+i]),small,y[i]));}}
 else{uint32_t large=mdkit_acc_to_word(mdkit_mpy(0x59999a,rattle));uint64_t carry=mdkit_from24(w[0x5f]);for(unsigned i=0;i<32;++i){w[0x5f+i]=mdkit_acc_to_word(carry);uint64_t input=mdkit_from24(y[0x40+i]);input=mdkit_asl(input,1);input=mdkit_mac(input,large,y[i]);carry=mdkit_mac(mdkit_from24(w[0x60+i]),small,y[i]);w[0x40+i]=mdkit_acc_to_word(input);}w[0x7f]=mdkit_acc_to_word(carry);}
 uint32_t gain=w[0xfb],cross=w[0xfc];
 unsigned first_cursor=s->delay_position[0],second_cursor=s->delay_position[1],third_cursor=s->delay_position[2];
 for(unsigned i=0;i<32;++i){
  uint64_t damped=mdkit_mac(mdkit_mpy(w[0x60+i],0x7eb852),cross,w[0xa0+i]);damped=mdkit_mac(damped,cross,w[0xc0+i]);y[i]=mdkit_acc_to_word(damped);
  uint64_t mix=mdkit_mac(mdkit_mpy(w[0x60+i],0x7eb852),gain,w[0x80+i]);s->delay_y[third_cursor]=mdkit_acc_to_word(mix);third_cursor=(third_cursor+1)&511;
  uint64_t opposite=mdkit_sub(mdkit_mpy(w[0x60+i],0x7eb852),mdkit_mpy(cross,w[0xa0+i]));opposite=mdkit_mac(opposite,cross,w[0xc0+i]);y[32+i]=mdkit_acc_to_word(opposite);
 }
 for(unsigned i=0;i<32;++i){s->delay_x[0][first_cursor]=y[(32)+i];first_cursor=(first_cursor+1)&511;s->delay_x[1][second_cursor]=y[i];second_cursor=(second_cursor+1)&511;}
 s->delay_position[0]=first_cursor;s->delay_position[1]=second_cursor;s->delay_position[2]=third_cursor;
}


static const uint16_t mdkit_pi_sd_word0_start[256]={
0,64,128,192,256,320,384,448,512,576,640,704,
768,832,896,960,1024,1088,1152,1216,1280,1344,1408,1472,
1536,1600,1664,1728,1792,1856,1920,1984,2048,2112,2176,2240,
2304,2368,2432,2496,2560,2624,2688,2752,2816,2880,2944,3008,
3072,3136,3200,3264,3328,3392,3456,3520,3584,3648,3712,3776,
3840,3904,3968,4032,4096,4160,4224,4288,4352,4416,4480,4544,
4608,4672,4736,4800,4864,4928,4992,5056,5120,5184,5248,5312,
5376,5440,5504,5568,5632,5696,5760,5824,5888,5952,6016,6080,
6144,6208,6272,6336,6400,6464,6528,6592,6656,6720,6784,6848,
6912,6976,7040,7104,7168,7232,7296,7360,7424,7488,7552,7616,
7680,7744,7808,7872,7936,8000,8064,8128,8192,8256,8320,8384,
8448,8512,8576,8640,8704,8768,8832,8896,8960,9024,9088,9152,
9216,9280,9344,9408,9472,9536,9600,9664,9728,9792,9856,9920,
9984,10048,10112,10176,10240,10304,10368,10432,10496,10560,10624,10688,
10752,10816,10880,10944,11008,11072,11136,11200,11264,11328,11392,11456,
11520,11584,11648,11712,11776,11840,11904,11968,12032,12096,12160,12224,
12288,12352,12416,12480,12544,12608,12672,12736,12800,12864,12928,12992,
13056,13120,13184,13248,13312,13376,13440,13504,13568,13632,13696,13760,
13824,13888,13952,14016,14080,14144,14208,14272,14336,14400,14464,14528,
14592,14656,14720,14784,14848,14912,14976,15040,15104,15168,15232,15296,
15360,15424,15488,15552,15616,15680,15744,15808,15872,15936,16000,16064,
16128,16192,16256,16320,
};
static const uint32_t mdkit_pi_sd_word0_value[256]={
2096152,2107416,2118680,2130968,2142232,2153496,2165784,2177048,2189336,2201624,2212888,2225176,
2237464,2249752,2262040,2274328,2286616,2298904,2312216,2324504,2336792,2350104,2362392,2375704,
2387992,2401304,2414616,2427928,2441240,2454552,2467864,2481176,2494488,2507800,2522136,2535448,
2549784,2563096,2577432,2591768,2605080,2619416,2633752,2648088,2662424,2677784,2692120,2706456,
2721816,2736152,2751512,2765848,2781208,2796568,2811928,2827288,2842648,2858008,2873368,2888728,
2905112,2920472,2936856,2953240,2968600,2984984,3001368,3017752,3034136,3050520,3066904,3084312,
3100696,3118104,3134488,3151896,3169304,3186712,3204120,3221528,3238936,3256344,3273752,3292184,
3309592,3328024,3346456,3364888,3382296,3400728,3420184,3438616,3457048,3476504,3494936,3514392,
3532824,3552280,3571736,3591192,3610648,3630104,3650584,3670040,3690520,3709976,3730456,3750936,
3771416,3791896,3812376,3833880,3854360,3875864,3896344,3917848,3939352,3960856,3982360,4003864,
4025368,4047896,4069400,4091928,4114456,4136984,4159512,4182040,4193304,4215832,4238360,4262936,
4285464,4307992,4332568,4355096,4379672,4404248,4426776,4451352,4475928,4500504,4525080,4549656,
4574232,4598808,4625432,4650008,4674584,4701208,4725784,4752408,4776984,4803608,4830232,4856856,
4883480,4910104,4936728,4963352,4989976,5016600,5045272,5071896,5100568,5127192,5155864,5184536,
5211160,5239832,5268504,5297176,5325848,5356568,5385240,5413912,5444632,5473304,5504024,5532696,
5563416,5594136,5624856,5655576,5686296,5717016,5747736,5778456,5811224,5841944,5874712,5907480,
5938200,5970968,6003736,6036504,6069272,6102040,6134808,6169624,6202392,6237208,6269976,6304792,
6339608,6374424,6409240,6444056,6478872,6513688,6548504,6585368,6620184,6657048,6693912,6730776,
6765592,6802456,6841368,6878232,6915096,6954008,6990872,7029784,7066648,7105560,7144472,7183384,
7222296,7261208,7302168,7341080,7382040,7420952,7461912,7502872,7543832,7584792,7625752,7668760,
7709720,7752728,7793688,7836696,7879704,7922712,7965720,8008728,8051736,8096792,8139800,8184856,
8229912,8274968,8320024,8365080,
};
static const uint32_t mdkit_pi_sd_word0_delta[256]={
0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,
};
static uint32_t mdkit_pi_sd_word0(unsigned raw){
 unsigned low=0,high=256;
 while(low+1<high){unsigned mid=(low+high)/2;if(mdkit_pi_sd_word0_start[mid]<=raw)low=mid;else high=mid;}
 return (mdkit_pi_sd_word0_value[low]+mdkit_pi_sd_word0_delta[low]*(raw-mdkit_pi_sd_word0_start[low]))&0xffffff;
}
static const uint16_t mdkit_pi_sd_word1_start[1]={
0,
};
static const uint32_t mdkit_pi_sd_word1_value[1]={
0,
};
static const uint32_t mdkit_pi_sd_word1_delta[1]={
512,
};
static uint32_t mdkit_pi_sd_word1(unsigned raw){
 unsigned low=0,high=1;
 while(low+1<high){unsigned mid=(low+high)/2;if(mdkit_pi_sd_word1_start[mid]<=raw)low=mid;else high=mid;}
 return (mdkit_pi_sd_word1_value[low]+mdkit_pi_sd_word1_delta[low]*(raw-mdkit_pi_sd_word1_start[low]))&0xffffff;
}
static const uint16_t mdkit_pi_sd_word2_start[1]={
0,
};
static const uint32_t mdkit_pi_sd_word2_value[1]={
0,
};
static const uint32_t mdkit_pi_sd_word2_delta[1]={
32,
};
static uint32_t mdkit_pi_sd_word2(unsigned raw){
 unsigned low=0,high=1;
 while(low+1<high){unsigned mid=(low+high)/2;if(mdkit_pi_sd_word2_start[mid]<=raw)low=mid;else high=mid;}
 return (mdkit_pi_sd_word2_value[low]+mdkit_pi_sd_word2_delta[low]*(raw-mdkit_pi_sd_word2_start[low]))&0xffffff;
}
static const uint16_t mdkit_pi_sd_word7_start[4097]={
0,6,8,10,12,22,24,26,28,38,40,42,
44,54,56,58,60,70,72,74,76,86,88,90,
92,102,104,106,108,118,120,122,124,134,136,138,
140,150,152,154,156,166,168,170,172,182,184,186,
188,198,200,202,204,214,216,218,220,230,232,234,
236,246,248,250,252,262,264,266,268,278,280,282,
284,294,296,298,300,310,312,314,316,326,328,330,
332,342,344,346,348,358,360,362,364,374,376,378,
380,390,392,394,396,406,408,410,412,422,424,426,
428,438,440,442,444,454,456,458,460,470,472,474,
476,486,488,490,492,502,504,506,508,518,520,522,
524,534,536,538,540,550,552,554,556,566,568,570,
572,582,584,586,588,598,600,602,604,614,616,618,
620,630,632,634,636,646,648,650,652,662,664,666,
668,678,680,682,684,694,696,698,700,710,712,714,
716,726,728,730,732,742,744,746,748,758,760,762,
764,774,776,778,780,790,792,794,796,806,808,810,
812,822,824,826,828,838,840,842,844,854,856,858,
860,870,872,874,876,886,888,890,892,902,904,906,
908,918,920,922,924,934,936,938,940,950,952,954,
956,966,968,970,972,982,984,986,988,998,1000,1002,
1004,1014,1016,1018,1020,1030,1032,1034,1036,1046,1048,1050,
1052,1062,1064,1066,1068,1078,1080,1082,1084,1094,1096,1098,
1100,1110,1112,1114,1116,1126,1128,1130,1132,1142,1144,1146,
1148,1158,1160,1162,1164,1174,1176,1178,1180,1190,1192,1194,
1196,1206,1208,1210,1212,1222,1224,1226,1228,1238,1240,1242,
1244,1254,1256,1258,1260,1270,1272,1274,1276,1286,1288,1290,
1292,1302,1304,1306,1308,1318,1320,1322,1324,1334,1336,1338,
1340,1350,1352,1354,1356,1366,1368,1370,1372,1382,1384,1386,
1388,1398,1400,1402,1404,1414,1416,1418,1420,1430,1432,1434,
1436,1446,1448,1450,1452,1462,1464,1466,1468,1478,1480,1482,
1484,1494,1496,1498,1500,1510,1512,1514,1516,1526,1528,1530,
1532,1542,1544,1546,1548,1558,1560,1562,1564,1574,1576,1578,
1580,1590,1592,1594,1596,1606,1608,1610,1612,1622,1624,1626,
1628,1638,1640,1642,1644,1654,1656,1658,1660,1670,1672,1674,
1676,1686,1688,1690,1692,1702,1704,1706,1708,1718,1720,1722,
1724,1734,1736,1738,1740,1750,1752,1754,1756,1766,1768,1770,
1772,1782,1784,1786,1788,1798,1800,1802,1804,1814,1816,1818,
1820,1830,1832,1834,1836,1846,1848,1850,1852,1862,1864,1866,
1868,1878,1880,1882,1884,1894,1896,1898,1900,1910,1912,1914,
1916,1926,1928,1930,1932,1942,1944,1946,1948,1958,1960,1962,
1964,1974,1976,1978,1980,1990,1992,1994,1996,2006,2008,2010,
2012,2022,2024,2026,2028,2038,2040,2042,2044,2054,2056,2058,
2060,2070,2072,2074,2076,2086,2088,2090,2092,2102,2104,2106,
2108,2118,2120,2122,2124,2134,2136,2138,2140,2150,2152,2154,
2156,2166,2168,2170,2172,2182,2184,2186,2188,2198,2200,2202,
2204,2214,2216,2218,2220,2230,2232,2234,2236,2246,2248,2250,
2252,2262,2264,2266,2268,2278,2280,2282,2284,2294,2296,2298,
2300,2310,2312,2314,2316,2326,2328,2330,2332,2342,2344,2346,
2348,2358,2360,2362,2364,2374,2376,2378,2380,2390,2392,2394,
2396,2406,2408,2410,2412,2422,2424,2426,2428,2438,2440,2442,
2444,2454,2456,2458,2460,2470,2472,2474,2476,2486,2488,2490,
2492,2502,2504,2506,2508,2518,2520,2522,2524,2534,2536,2538,
2540,2550,2552,2554,2556,2566,2568,2570,2572,2582,2584,2586,
2588,2598,2600,2602,2604,2614,2616,2618,2620,2630,2632,2634,
2636,2646,2648,2650,2652,2662,2664,2666,2668,2678,2680,2682,
2684,2694,2696,2698,2700,2710,2712,2714,2716,2726,2728,2730,
2732,2742,2744,2746,2748,2758,2760,2762,2764,2774,2776,2778,
2780,2790,2792,2794,2796,2806,2808,2810,2812,2822,2824,2826,
2828,2838,2840,2842,2844,2854,2856,2858,2860,2870,2872,2874,
2876,2886,2888,2890,2892,2902,2904,2906,2908,2918,2920,2922,
2924,2934,2936,2938,2940,2950,2952,2954,2956,2966,2968,2970,
2972,2982,2984,2986,2988,2998,3000,3002,3004,3014,3016,3018,
3020,3030,3032,3034,3036,3046,3048,3050,3052,3062,3064,3066,
3068,3078,3080,3082,3084,3094,3096,3098,3100,3110,3112,3114,
3116,3126,3128,3130,3132,3142,3144,3146,3148,3158,3160,3162,
3164,3174,3176,3178,3180,3190,3192,3194,3196,3206,3208,3210,
3212,3222,3224,3226,3228,3238,3240,3242,3244,3254,3256,3258,
3260,3270,3272,3274,3276,3286,3288,3290,3292,3302,3304,3306,
3308,3318,3320,3322,3324,3334,3336,3338,3340,3350,3352,3354,
3356,3366,3368,3370,3372,3382,3384,3386,3388,3398,3400,3402,
3404,3414,3416,3418,3420,3430,3432,3434,3436,3446,3448,3450,
3452,3462,3464,3466,3468,3478,3480,3482,3484,3494,3496,3498,
3500,3510,3512,3514,3516,3526,3528,3530,3532,3542,3544,3546,
3548,3558,3560,3562,3564,3574,3576,3578,3580,3590,3592,3594,
3596,3606,3608,3610,3612,3622,3624,3626,3628,3638,3640,3642,
3644,3654,3656,3658,3660,3670,3672,3674,3676,3686,3688,3690,
3692,3702,3704,3706,3708,3718,3720,3722,3724,3734,3736,3738,
3740,3750,3752,3754,3756,3766,3768,3770,3772,3782,3784,3786,
3788,3798,3800,3802,3804,3814,3816,3818,3820,3830,3832,3834,
3836,3846,3848,3850,3852,3862,3864,3866,3868,3878,3880,3882,
3884,3894,3896,3898,3900,3910,3912,3914,3916,3926,3928,3930,
3932,3942,3944,3946,3948,3958,3960,3962,3964,3974,3976,3978,
3980,3990,3992,3994,3996,4006,4008,4010,4012,4022,4024,4026,
4028,4038,4040,4042,4044,4054,4056,4058,4060,4070,4072,4074,
4076,4086,4088,4090,4092,4102,4104,4106,4108,4118,4120,4122,
4124,4134,4136,4138,4140,4150,4152,4154,4156,4166,4168,4170,
4172,4182,4184,4186,4188,4198,4200,4202,4204,4214,4216,4218,
4220,4230,4232,4234,4236,4246,4248,4250,4252,4262,4264,4266,
4268,4278,4280,4282,4284,4294,4296,4298,4300,4310,4312,4314,
4316,4326,4328,4330,4332,4342,4344,4346,4348,4358,4360,4362,
4364,4374,4376,4378,4380,4390,4392,4394,4396,4406,4408,4410,
4412,4422,4424,4426,4428,4438,4440,4442,4444,4454,4456,4458,
4460,4470,4472,4474,4476,4486,4488,4490,4492,4502,4504,4506,
4508,4518,4520,4522,4524,4534,4536,4538,4540,4550,4552,4554,
4556,4566,4568,4570,4572,4582,4584,4586,4588,4598,4600,4602,
4604,4614,4616,4618,4620,4630,4632,4634,4636,4646,4648,4650,
4652,4662,4664,4666,4668,4678,4680,4682,4684,4694,4696,4698,
4700,4710,4712,4714,4716,4726,4728,4730,4732,4742,4744,4746,
4748,4758,4760,4762,4764,4774,4776,4778,4780,4790,4792,4794,
4796,4806,4808,4810,4812,4822,4824,4826,4828,4838,4840,4842,
4844,4854,4856,4858,4860,4870,4872,4874,4876,4886,4888,4890,
4892,4902,4904,4906,4908,4918,4920,4922,4924,4934,4936,4938,
4940,4950,4952,4954,4956,4966,4968,4970,4972,4982,4984,4986,
4988,4998,5000,5002,5004,5014,5016,5018,5020,5030,5032,5034,
5036,5046,5048,5050,5052,5062,5064,5066,5068,5078,5080,5082,
5084,5094,5096,5098,5100,5110,5112,5114,5116,5126,5128,5130,
5132,5142,5144,5146,5148,5158,5160,5162,5164,5174,5176,5178,
5180,5190,5192,5194,5196,5206,5208,5210,5212,5222,5224,5226,
5228,5238,5240,5242,5244,5254,5256,5258,5260,5270,5272,5274,
5276,5286,5288,5290,5292,5302,5304,5306,5308,5318,5320,5322,
5324,5334,5336,5338,5340,5350,5352,5354,5356,5366,5368,5370,
5372,5382,5384,5386,5388,5398,5400,5402,5404,5414,5416,5418,
5420,5430,5432,5434,5436,5446,5448,5450,5452,5462,5464,5466,
5468,5478,5480,5482,5484,5494,5496,5498,5500,5510,5512,5514,
5516,5526,5528,5530,5532,5542,5544,5546,5548,5558,5560,5562,
5564,5574,5576,5578,5580,5590,5592,5594,5596,5606,5608,5610,
5612,5622,5624,5626,5628,5638,5640,5642,5644,5654,5656,5658,
5660,5670,5672,5674,5676,5686,5688,5690,5692,5702,5704,5706,
5708,5718,5720,5722,5724,5734,5736,5738,5740,5750,5752,5754,
5756,5766,5768,5770,5772,5782,5784,5786,5788,5798,5800,5802,
5804,5814,5816,5818,5820,5830,5832,5834,5836,5846,5848,5850,
5852,5862,5864,5866,5868,5878,5880,5882,5884,5894,5896,5898,
5900,5910,5912,5914,5916,5926,5928,5930,5932,5942,5944,5946,
5948,5958,5960,5962,5964,5974,5976,5978,5980,5990,5992,5994,
5996,6006,6008,6010,6012,6022,6024,6026,6028,6038,6040,6042,
6044,6054,6056,6058,6060,6070,6072,6074,6076,6086,6088,6090,
6092,6102,6104,6106,6108,6118,6120,6122,6124,6134,6136,6138,
6140,6150,6152,6154,6156,6166,6168,6170,6172,6182,6184,6186,
6188,6198,6200,6202,6204,6214,6216,6218,6220,6230,6232,6234,
6236,6246,6248,6250,6252,6262,6264,6266,6268,6278,6280,6282,
6284,6294,6296,6298,6300,6310,6312,6314,6316,6326,6328,6330,
6332,6342,6344,6346,6348,6358,6360,6362,6364,6374,6376,6378,
6380,6390,6392,6394,6396,6406,6408,6410,6412,6422,6424,6426,
6428,6438,6440,6442,6444,6454,6456,6458,6460,6470,6472,6474,
6476,6486,6488,6490,6492,6502,6504,6506,6508,6518,6520,6522,
6524,6534,6536,6538,6540,6550,6552,6554,6556,6566,6568,6570,
6572,6582,6584,6586,6588,6598,6600,6602,6604,6614,6616,6618,
6620,6630,6632,6634,6636,6646,6648,6650,6652,6662,6664,6666,
6668,6678,6680,6682,6684,6694,6696,6698,6700,6710,6712,6714,
6716,6726,6728,6730,6732,6742,6744,6746,6748,6758,6760,6762,
6764,6774,6776,6778,6780,6790,6792,6794,6796,6806,6808,6810,
6812,6822,6824,6826,6828,6838,6840,6842,6844,6854,6856,6858,
6860,6870,6872,6874,6876,6886,6888,6890,6892,6902,6904,6906,
6908,6918,6920,6922,6924,6934,6936,6938,6940,6950,6952,6954,
6956,6966,6968,6970,6972,6982,6984,6986,6988,6998,7000,7002,
7004,7014,7016,7018,7020,7030,7032,7034,7036,7046,7048,7050,
7052,7062,7064,7066,7068,7078,7080,7082,7084,7094,7096,7098,
7100,7110,7112,7114,7116,7126,7128,7130,7132,7142,7144,7146,
7148,7158,7160,7162,7164,7174,7176,7178,7180,7190,7192,7194,
7196,7206,7208,7210,7212,7222,7224,7226,7228,7238,7240,7242,
7244,7254,7256,7258,7260,7270,7272,7274,7276,7286,7288,7290,
7292,7302,7304,7306,7308,7318,7320,7322,7324,7334,7336,7338,
7340,7350,7352,7354,7356,7366,7368,7370,7372,7382,7384,7386,
7388,7398,7400,7402,7404,7414,7416,7418,7420,7430,7432,7434,
7436,7446,7448,7450,7452,7462,7464,7466,7468,7478,7480,7482,
7484,7494,7496,7498,7500,7510,7512,7514,7516,7526,7528,7530,
7532,7542,7544,7546,7548,7558,7560,7562,7564,7574,7576,7578,
7580,7590,7592,7594,7596,7606,7608,7610,7612,7622,7624,7626,
7628,7638,7640,7642,7644,7654,7656,7658,7660,7670,7672,7674,
7676,7686,7688,7690,7692,7702,7704,7706,7708,7718,7720,7722,
7724,7734,7736,7738,7740,7750,7752,7754,7756,7766,7768,7770,
7772,7782,7784,7786,7788,7798,7800,7802,7804,7814,7816,7818,
7820,7830,7832,7834,7836,7846,7848,7850,7852,7862,7864,7866,
7868,7878,7880,7882,7884,7894,7896,7898,7900,7910,7912,7914,
7916,7926,7928,7930,7932,7942,7944,7946,7948,7958,7960,7962,
7964,7974,7976,7978,7980,7990,7992,7994,7996,8006,8008,8010,
8012,8022,8024,8026,8028,8038,8040,8042,8044,8054,8056,8058,
8060,8070,8072,8074,8076,8086,8088,8090,8092,8102,8104,8106,
8108,8118,8120,8122,8124,8134,8136,8138,8140,8150,8152,8154,
8156,8166,8168,8170,8172,8182,8184,8186,8188,8198,8200,8202,
8204,8214,8216,8218,8220,8230,8232,8234,8236,8246,8248,8250,
8252,8262,8264,8266,8268,8278,8280,8282,8284,8294,8296,8298,
8300,8310,8312,8314,8316,8326,8328,8330,8332,8342,8344,8346,
8348,8358,8360,8362,8364,8374,8376,8378,8380,8390,8392,8394,
8396,8406,8408,8410,8412,8422,8424,8426,8428,8438,8440,8442,
8444,8454,8456,8458,8460,8470,8472,8474,8476,8486,8488,8490,
8492,8502,8504,8506,8508,8518,8520,8522,8524,8534,8536,8538,
8540,8550,8552,8554,8556,8566,8568,8570,8572,8582,8584,8586,
8588,8598,8600,8602,8604,8614,8616,8618,8620,8630,8632,8634,
8636,8646,8648,8650,8652,8662,8664,8666,8668,8678,8680,8682,
8684,8694,8696,8698,8700,8710,8712,8714,8716,8726,8728,8730,
8732,8742,8744,8746,8748,8758,8760,8762,8764,8774,8776,8778,
8780,8790,8792,8794,8796,8806,8808,8810,8812,8822,8824,8826,
8828,8838,8840,8842,8844,8854,8856,8858,8860,8870,8872,8874,
8876,8886,8888,8890,8892,8902,8904,8906,8908,8918,8920,8922,
8924,8934,8936,8938,8940,8950,8952,8954,8956,8966,8968,8970,
8972,8982,8984,8986,8988,8998,9000,9002,9004,9014,9016,9018,
9020,9030,9032,9034,9036,9046,9048,9050,9052,9062,9064,9066,
9068,9078,9080,9082,9084,9094,9096,9098,9100,9110,9112,9114,
9116,9126,9128,9130,9132,9142,9144,9146,9148,9158,9160,9162,
9164,9174,9176,9178,9180,9190,9192,9194,9196,9206,9208,9210,
9212,9222,9224,9226,9228,9238,9240,9242,9244,9254,9256,9258,
9260,9270,9272,9274,9276,9286,9288,9290,9292,9302,9304,9306,
9308,9318,9320,9322,9324,9334,9336,9338,9340,9350,9352,9354,
9356,9366,9368,9370,9372,9382,9384,9386,9388,9398,9400,9402,
9404,9414,9416,9418,9420,9430,9432,9434,9436,9446,9448,9450,
9452,9462,9464,9466,9468,9478,9480,9482,9484,9494,9496,9498,
9500,9510,9512,9514,9516,9526,9528,9530,9532,9542,9544,9546,
9548,9558,9560,9562,9564,9574,9576,9578,9580,9590,9592,9594,
9596,9606,9608,9610,9612,9622,9624,9626,9628,9638,9640,9642,
9644,9654,9656,9658,9660,9670,9672,9674,9676,9686,9688,9690,
9692,9702,9704,9706,9708,9718,9720,9722,9724,9734,9736,9738,
9740,9750,9752,9754,9756,9766,9768,9770,9772,9782,9784,9786,
9788,9798,9800,9802,9804,9814,9816,9818,9820,9830,9832,9834,
9836,9846,9848,9850,9852,9862,9864,9866,9868,9878,9880,9882,
9884,9894,9896,9898,9900,9910,9912,9914,9916,9926,9928,9930,
9932,9942,9944,9946,9948,9958,9960,9962,9964,9974,9976,9978,
9980,9990,9992,9994,9996,10006,10008,10010,10012,10022,10024,10026,
10028,10038,10040,10042,10044,10054,10056,10058,10060,10070,10072,10074,
10076,10086,10088,10090,10092,10102,10104,10106,10108,10118,10120,10122,
10124,10134,10136,10138,10140,10150,10152,10154,10156,10166,10168,10170,
10172,10182,10184,10186,10188,10198,10200,10202,10204,10214,10216,10218,
10220,10230,10232,10234,10236,10246,10248,10250,10252,10262,10264,10266,
10268,10278,10280,10282,10284,10294,10296,10298,10300,10310,10312,10314,
10316,10326,10328,10330,10332,10342,10344,10346,10348,10358,10360,10362,
10364,10374,10376,10378,10380,10390,10392,10394,10396,10406,10408,10410,
10412,10422,10424,10426,10428,10438,10440,10442,10444,10454,10456,10458,
10460,10470,10472,10474,10476,10486,10488,10490,10492,10502,10504,10506,
10508,10518,10520,10522,10524,10534,10536,10538,10540,10550,10552,10554,
10556,10566,10568,10570,10572,10582,10584,10586,10588,10598,10600,10602,
10604,10614,10616,10618,10620,10630,10632,10634,10636,10646,10648,10650,
10652,10662,10664,10666,10668,10678,10680,10682,10684,10694,10696,10698,
10700,10710,10712,10714,10716,10726,10728,10730,10732,10742,10744,10746,
10748,10758,10760,10762,10764,10774,10776,10778,10780,10790,10792,10794,
10796,10806,10808,10810,10812,10822,10824,10826,10828,10838,10840,10842,
10844,10854,10856,10858,10860,10870,10872,10874,10876,10886,10888,10890,
10892,10902,10904,10906,10908,10918,10920,10922,10924,10934,10936,10938,
10940,10950,10952,10954,10956,10966,10968,10970,10972,10982,10984,10986,
10988,10998,11000,11002,11004,11014,11016,11018,11020,11030,11032,11034,
11036,11046,11048,11050,11052,11062,11064,11066,11068,11078,11080,11082,
11084,11094,11096,11098,11100,11110,11112,11114,11116,11126,11128,11130,
11132,11142,11144,11146,11148,11158,11160,11162,11164,11174,11176,11178,
11180,11190,11192,11194,11196,11206,11208,11210,11212,11222,11224,11226,
11228,11238,11240,11242,11244,11254,11256,11258,11260,11270,11272,11274,
11276,11286,11288,11290,11292,11302,11304,11306,11308,11318,11320,11322,
11324,11334,11336,11338,11340,11350,11352,11354,11356,11366,11368,11370,
11372,11382,11384,11386,11388,11398,11400,11402,11404,11414,11416,11418,
11420,11430,11432,11434,11436,11446,11448,11450,11452,11462,11464,11466,
11468,11478,11480,11482,11484,11494,11496,11498,11500,11510,11512,11514,
11516,11526,11528,11530,11532,11542,11544,11546,11548,11558,11560,11562,
11564,11574,11576,11578,11580,11590,11592,11594,11596,11606,11608,11610,
11612,11622,11624,11626,11628,11638,11640,11642,11644,11654,11656,11658,
11660,11670,11672,11674,11676,11686,11688,11690,11692,11702,11704,11706,
11708,11718,11720,11722,11724,11734,11736,11738,11740,11750,11752,11754,
11756,11766,11768,11770,11772,11782,11784,11786,11788,11798,11800,11802,
11804,11814,11816,11818,11820,11830,11832,11834,11836,11846,11848,11850,
11852,11862,11864,11866,11868,11878,11880,11882,11884,11894,11896,11898,
11900,11910,11912,11914,11916,11926,11928,11930,11932,11942,11944,11946,
11948,11958,11960,11962,11964,11974,11976,11978,11980,11990,11992,11994,
11996,12006,12008,12010,12012,12022,12024,12026,12028,12038,12040,12042,
12044,12054,12056,12058,12060,12070,12072,12074,12076,12086,12088,12090,
12092,12102,12104,12106,12108,12118,12120,12122,12124,12134,12136,12138,
12140,12150,12152,12154,12156,12166,12168,12170,12172,12182,12184,12186,
12188,12198,12200,12202,12204,12214,12216,12218,12220,12230,12232,12234,
12236,12246,12248,12250,12252,12262,12264,12266,12268,12278,12280,12282,
12284,12294,12296,12298,12300,12310,12312,12314,12316,12326,12328,12330,
12332,12342,12344,12346,12348,12358,12360,12362,12364,12374,12376,12378,
12380,12390,12392,12394,12396,12406,12408,12410,12412,12422,12424,12426,
12428,12438,12440,12442,12444,12454,12456,12458,12460,12470,12472,12474,
12476,12486,12488,12490,12492,12502,12504,12506,12508,12518,12520,12522,
12524,12534,12536,12538,12540,12550,12552,12554,12556,12566,12568,12570,
12572,12582,12584,12586,12588,12598,12600,12602,12604,12614,12616,12618,
12620,12630,12632,12634,12636,12646,12648,12650,12652,12662,12664,12666,
12668,12678,12680,12682,12684,12694,12696,12698,12700,12710,12712,12714,
12716,12726,12728,12730,12732,12742,12744,12746,12748,12758,12760,12762,
12764,12774,12776,12778,12780,12790,12792,12794,12796,12806,12808,12810,
12812,12822,12824,12826,12828,12838,12840,12842,12844,12854,12856,12858,
12860,12870,12872,12874,12876,12886,12888,12890,12892,12902,12904,12906,
12908,12918,12920,12922,12924,12934,12936,12938,12940,12950,12952,12954,
12956,12966,12968,12970,12972,12982,12984,12986,12988,12998,13000,13002,
13004,13014,13016,13018,13020,13030,13032,13034,13036,13046,13048,13050,
13052,13062,13064,13066,13068,13078,13080,13082,13084,13094,13096,13098,
13100,13110,13112,13114,13116,13126,13128,13130,13132,13142,13144,13146,
13148,13158,13160,13162,13164,13174,13176,13178,13180,13190,13192,13194,
13196,13206,13208,13210,13212,13222,13224,13226,13228,13238,13240,13242,
13244,13254,13256,13258,13260,13270,13272,13274,13276,13286,13288,13290,
13292,13302,13304,13306,13308,13318,13320,13322,13324,13334,13336,13338,
13340,13350,13352,13354,13356,13366,13368,13370,13372,13382,13384,13386,
13388,13398,13400,13402,13404,13414,13416,13418,13420,13430,13432,13434,
13436,13446,13448,13450,13452,13462,13464,13466,13468,13478,13480,13482,
13484,13494,13496,13498,13500,13510,13512,13514,13516,13526,13528,13530,
13532,13542,13544,13546,13548,13558,13560,13562,13564,13574,13576,13578,
13580,13590,13592,13594,13596,13606,13608,13610,13612,13622,13624,13626,
13628,13638,13640,13642,13644,13654,13656,13658,13660,13670,13672,13674,
13676,13686,13688,13690,13692,13702,13704,13706,13708,13718,13720,13722,
13724,13734,13736,13738,13740,13750,13752,13754,13756,13766,13768,13770,
13772,13782,13784,13786,13788,13798,13800,13802,13804,13814,13816,13818,
13820,13830,13832,13834,13836,13846,13848,13850,13852,13862,13864,13866,
13868,13878,13880,13882,13884,13894,13896,13898,13900,13910,13912,13914,
13916,13926,13928,13930,13932,13942,13944,13946,13948,13958,13960,13962,
13964,13974,13976,13978,13980,13990,13992,13994,13996,14006,14008,14010,
14012,14022,14024,14026,14028,14038,14040,14042,14044,14054,14056,14058,
14060,14070,14072,14074,14076,14086,14088,14090,14092,14102,14104,14106,
14108,14118,14120,14122,14124,14134,14136,14138,14140,14150,14152,14154,
14156,14166,14168,14170,14172,14182,14184,14186,14188,14198,14200,14202,
14204,14214,14216,14218,14220,14230,14232,14234,14236,14246,14248,14250,
14252,14262,14264,14266,14268,14278,14280,14282,14284,14294,14296,14298,
14300,14310,14312,14314,14316,14326,14328,14330,14332,14342,14344,14346,
14348,14358,14360,14362,14364,14374,14376,14378,14380,14390,14392,14394,
14396,14406,14408,14410,14412,14422,14424,14426,14428,14438,14440,14442,
14444,14454,14456,14458,14460,14470,14472,14474,14476,14486,14488,14490,
14492,14502,14504,14506,14508,14518,14520,14522,14524,14534,14536,14538,
14540,14550,14552,14554,14556,14566,14568,14570,14572,14582,14584,14586,
14588,14598,14600,14602,14604,14614,14616,14618,14620,14630,14632,14634,
14636,14646,14648,14650,14652,14662,14664,14666,14668,14678,14680,14682,
14684,14694,14696,14698,14700,14710,14712,14714,14716,14726,14728,14730,
14732,14742,14744,14746,14748,14758,14760,14762,14764,14774,14776,14778,
14780,14790,14792,14794,14796,14806,14808,14810,14812,14822,14824,14826,
14828,14838,14840,14842,14844,14854,14856,14858,14860,14870,14872,14874,
14876,14886,14888,14890,14892,14902,14904,14906,14908,14918,14920,14922,
14924,14934,14936,14938,14940,14950,14952,14954,14956,14966,14968,14970,
14972,14982,14984,14986,14988,14998,15000,15002,15004,15014,15016,15018,
15020,15030,15032,15034,15036,15046,15048,15050,15052,15062,15064,15066,
15068,15078,15080,15082,15084,15094,15096,15098,15100,15110,15112,15114,
15116,15126,15128,15130,15132,15142,15144,15146,15148,15158,15160,15162,
15164,15174,15176,15178,15180,15190,15192,15194,15196,15206,15208,15210,
15212,15222,15224,15226,15228,15238,15240,15242,15244,15254,15256,15258,
15260,15270,15272,15274,15276,15286,15288,15290,15292,15302,15304,15306,
15308,15318,15320,15322,15324,15334,15336,15338,15340,15350,15352,15354,
15356,15366,15368,15370,15372,15382,15384,15386,15388,15398,15400,15402,
15404,15414,15416,15418,15420,15430,15432,15434,15436,15446,15448,15450,
15452,15462,15464,15466,15468,15478,15480,15482,15484,15494,15496,15498,
15500,15510,15512,15514,15516,15526,15528,15530,15532,15542,15544,15546,
15548,15558,15560,15562,15564,15574,15576,15578,15580,15590,15592,15594,
15596,15606,15608,15610,15612,15622,15624,15626,15628,15638,15640,15642,
15644,15654,15656,15658,15660,15670,15672,15674,15676,15686,15688,15690,
15692,15702,15704,15706,15708,15718,15720,15722,15724,15734,15736,15738,
15740,15750,15752,15754,15756,15766,15768,15770,15772,15782,15784,15786,
15788,15798,15800,15802,15804,15814,15816,15818,15820,15830,15832,15834,
15836,15846,15848,15850,15852,15862,15864,15866,15868,15878,15880,15882,
15884,15894,15896,15898,15900,15910,15912,15914,15916,15926,15928,15930,
15932,15942,15944,15946,15948,15958,15960,15962,15964,15974,15976,15978,
15980,15990,15992,15994,15996,16006,16008,16010,16012,16022,16024,16026,
16028,16038,16040,16042,16044,16054,16056,16058,16060,16070,16072,16074,
16076,16086,16088,16090,16092,16102,16104,16106,16108,16118,16120,16122,
16124,16134,16136,16138,16140,16150,16152,16154,16156,16166,16168,16170,
16172,16182,16184,16186,16188,16198,16200,16202,16204,16214,16216,16218,
16220,16230,16232,16234,16236,16246,16248,16250,16252,16262,16264,16266,
16268,16278,16280,16282,16284,16294,16296,16298,16300,16310,16312,16314,
16316,16326,16328,16330,16332,16342,16344,16346,16348,16358,16360,16362,
16364,16374,16376,16378,16380,
};
static const uint32_t mdkit_pi_sd_word7_value[4097]={
0,1,2,3,4,15,18,21,24,45,50,55,
60,91,98,105,112,153,162,171,180,231,242,253,
264,325,338,351,364,435,450,465,480,561,578,595,
612,703,722,741,760,861,882,903,924,1035,1058,1081,
1104,1225,1250,1275,1300,1431,1458,1485,1512,1653,1682,1711,
1740,1891,1922,1953,1984,2145,2178,2211,2244,2415,2450,2485,
2520,2701,2738,2775,2812,3003,3042,3081,3120,3321,3362,3403,
3444,3655,3698,3741,3784,4005,4050,4095,4140,4371,4418,4465,
4512,4753,4802,4851,4900,5151,5202,5253,5304,5565,5618,5671,
5724,5995,6050,6105,6160,6441,6498,6555,6612,6903,6962,7021,
7080,7381,7442,7503,7564,7875,7938,8001,8064,8385,8450,8515,
8580,8911,8978,9045,9112,9453,9522,9591,9660,10011,10082,10153,
10224,10585,10658,10731,10804,11175,11250,11325,11400,11781,11858,11935,
12012,12403,12482,12561,12640,13041,13122,13203,13284,13695,13778,13861,
13944,14365,14450,14535,14620,15051,15138,15225,15312,15753,15842,15931,
16020,16471,16562,16653,16744,17205,17298,17391,17484,17955,18050,18145,
18240,18721,18818,18915,19012,19503,19602,19701,19800,20301,20402,20503,
20604,21115,21218,21321,21424,21945,22050,22155,22260,22791,22898,23005,
23112,23653,23762,23871,23980,24531,24642,24753,24864,25425,25538,25651,
25764,26335,26450,26565,26680,27261,27378,27495,27612,28203,28322,28441,
28560,29161,29282,29403,29524,30135,30258,30381,30504,31125,31250,31375,
31500,32131,32258,32385,32512,33153,33282,33411,33540,34191,34322,34453,
34584,35245,35378,35511,35644,36315,36450,36585,36720,37401,37538,37675,
37812,38503,38642,38781,38920,39621,39762,39903,40044,40755,40898,41041,
41184,41905,42050,42195,42340,43071,43218,43365,43512,44253,44402,44551,
44700,45451,45602,45753,45904,46665,46818,46971,47124,47895,48050,48205,
48360,49141,49298,49455,49612,50403,50562,50721,50880,51681,51842,52003,
52164,52975,53138,53301,53464,54285,54450,54615,54780,55611,55778,55945,
56112,56953,57122,57291,57460,58311,58482,58653,58824,59685,59858,60031,
60204,61075,61250,61425,61600,62481,62658,62835,63012,63903,64082,64261,
64440,65341,65522,65703,65884,66795,66978,67161,67344,68265,68450,68635,
68820,69751,69938,70125,70312,71253,71442,71631,71820,72771,72962,73153,
73344,74305,74498,74691,74884,75855,76050,76245,76440,77421,77618,77815,
78012,79003,79202,79401,79600,80601,80802,81003,81204,82215,82418,82621,
82824,83845,84050,84255,84460,85491,85698,85905,86112,87153,87362,87571,
87780,88831,89042,89253,89464,90525,90738,90951,91164,92235,92450,92665,
92880,93961,94178,94395,94612,95703,95922,96141,96360,97461,97682,97903,
98124,99235,99458,99681,99904,101025,101250,101475,101700,102831,103058,103285,
103512,104653,104882,105111,105340,106491,106722,106953,107184,108345,108578,108811,
109044,110215,110450,110685,110920,112101,112338,112575,112812,114003,114242,114481,
114720,115921,116162,116403,116644,117855,118098,118341,118584,119805,120050,120295,
120540,121771,122018,122265,122512,123753,124002,124251,124500,125751,126002,126253,
126504,127765,128018,128271,128524,129795,130050,130305,130560,131841,132098,132355,
132612,133903,134162,134421,134680,135981,136242,136503,136764,138075,138338,138601,
138864,140185,140450,140715,140980,142311,142578,142845,143112,144453,144722,144991,
145260,146611,146882,147153,147424,148785,149058,149331,149604,150975,151250,151525,
151800,153181,153458,153735,154012,155403,155682,155961,156240,157641,157922,158203,
158484,159895,160178,160461,160744,162165,162450,162735,163020,164451,164738,165025,
165312,166753,167042,167331,167620,169071,169362,169653,169944,171405,171698,171991,
172284,173755,174050,174345,174640,176121,176418,176715,177012,178503,178802,179101,
179400,180901,181202,181503,181804,183315,183618,183921,184224,185745,186050,186355,
186660,188191,188498,188805,189112,190653,190962,191271,191580,193131,193442,193753,
194064,195625,195938,196251,196564,198135,198450,198765,199080,200661,200978,201295,
201612,203203,203522,203841,204160,205761,206082,206403,206724,208335,208658,208981,
209304,210925,211250,211575,211900,213531,213858,214185,214512,216153,216482,216811,
217140,218791,219122,219453,219784,221445,221778,222111,222444,224115,224450,224785,
225120,226801,227138,227475,227812,229503,229842,230181,230520,232221,232562,232903,
233244,234955,235298,235641,235984,237705,238050,238395,238740,240471,240818,241165,
241512,243253,243602,243951,244300,246051,246402,246753,247104,248865,249218,249571,
249924,251695,252050,252405,252760,254541,254898,255255,255612,257403,257762,258121,
258480,260281,260642,261003,261364,263175,263538,263901,264264,266085,266450,266815,
267180,269011,269378,269745,270112,271953,272322,272691,273060,274911,275282,275653,
276024,277885,278258,278631,279004,280875,281250,281625,282000,283881,284258,284635,
285012,286903,287282,287661,288040,289941,290322,290703,291084,292995,293378,293761,
294144,296065,296450,296835,297220,299151,299538,299925,300312,302253,302642,303031,
303420,305371,305762,306153,306544,308505,308898,309291,309684,311655,312050,312445,
312840,314821,315218,315615,316012,318003,318402,318801,319200,321201,321602,322003,
322404,324415,324818,325221,325624,327645,328050,328455,328860,330891,331298,331705,
332112,334153,334562,334971,335380,337431,337842,338253,338664,340725,341138,341551,
341964,344035,344450,344865,345280,347361,347778,348195,348612,350703,351122,351541,
351960,354061,354482,354903,355324,357435,357858,358281,358704,360825,361250,361675,
362100,364231,364658,365085,365512,367653,368082,368511,368940,371091,371522,371953,
372384,374545,374978,375411,375844,378015,378450,378885,379320,381501,381938,382375,
382812,385003,385442,385881,386320,388521,388962,389403,389844,392055,392498,392941,
393384,395605,396050,396495,396940,399171,399618,400065,400512,402753,403202,403651,
404100,406351,406802,407253,407704,409965,410418,410871,411324,413595,414050,414505,
414960,417241,417698,418155,418612,420903,421362,421821,422280,424581,425042,425503,
425964,428275,428738,429201,429664,431985,432450,432915,433380,435711,436178,436645,
437112,439453,439922,440391,440860,443211,443682,444153,444624,446985,447458,447931,
448404,450775,451250,451725,452200,454581,455058,455535,456012,458403,458882,459361,
459840,462241,462722,463203,463684,466095,466578,467061,467544,469965,470450,470935,
471420,473851,474338,474825,475312,477753,478242,478731,479220,481671,482162,482653,
483144,485605,486098,486591,487084,489555,490050,490545,491040,493521,494018,494515,
495012,497503,498002,498501,499000,501501,502002,502503,503004,505515,506018,506521,
507024,509545,510050,510555,511060,513591,514098,514605,515112,517653,518162,518671,
519180,521731,522242,522753,523264,525825,526338,526851,527364,529935,530450,530965,
531480,534061,534578,535095,535612,538203,538722,539241,539760,542361,542882,543403,
543924,546535,547058,547581,548104,550725,551250,551775,552300,554931,555458,555985,
556512,559153,559682,560211,560740,563391,563922,564453,564984,567645,568178,568711,
569244,571915,572450,572985,573520,576201,576738,577275,577812,580503,581042,581581,
582120,584821,585362,585903,586444,589155,589698,590241,590784,593505,594050,594595,
595140,597871,598418,598965,599512,602253,602802,603351,603900,606651,607202,607753,
608304,611065,611618,612171,612724,615495,616050,616605,617160,619941,620498,621055,
621612,624403,624962,625521,626080,628881,629442,630003,630564,633375,633938,634501,
635064,637885,638450,639015,639580,642411,642978,643545,644112,646953,647522,648091,
648660,651511,652082,652653,653224,656085,656658,657231,657804,660675,661250,661825,
662400,665281,665858,666435,667012,669903,670482,671061,671640,674541,675122,675703,
676284,679195,679778,680361,680944,683865,684450,685035,685620,688551,689138,689725,
690312,693253,693842,694431,695020,697971,698562,699153,699744,702705,703298,703891,
704484,707455,708050,708645,709240,712221,712818,713415,714012,717003,717602,718201,
718800,721801,722402,723003,723604,726615,727218,727821,728424,731445,732050,732655,
733260,736291,736898,737505,738112,741153,741762,742371,742980,746031,746642,747253,
747864,750925,751538,752151,752764,755835,756450,757065,757680,760761,761378,761995,
762612,765703,766322,766941,767560,770661,771282,771903,772524,775635,776258,776881,
777504,780625,781250,781875,782500,785631,786258,786885,787512,790653,791282,791911,
792540,795691,796322,796953,797584,800745,801378,802011,802644,805815,806450,807085,
807720,810901,811538,812175,812812,816003,816642,817281,817920,821121,821762,822403,
823044,826255,826898,827541,828184,831405,832050,832695,833340,836571,837218,837865,
838512,841753,842402,843051,843700,846951,847602,848253,848904,852165,852818,853471,
854124,857395,858050,858705,859360,862641,863298,863955,864612,867903,868562,869221,
869880,873181,873842,874503,875164,878475,879138,879801,880464,883785,884450,885115,
885780,889111,889778,890445,891112,894453,895122,895791,896460,899811,900482,901153,
901824,905185,905858,906531,907204,910575,911250,911925,912600,915981,916658,917335,
918012,921403,922082,922761,923440,926841,927522,928203,928884,932295,932978,933661,
934344,937765,938450,939135,939820,943251,943938,944625,945312,948753,949442,950131,
950820,954271,954962,955653,956344,959805,960498,961191,961884,965355,966050,966745,
967440,970921,971618,972315,973012,976503,977202,977901,978600,982101,982802,983503,
984204,987715,988418,989121,989824,993345,994050,994755,995460,998991,999698,1000405,
1001112,1004653,1005362,1006071,1006780,1010331,1011042,1011753,1012464,1016025,1016738,1017451,
1018164,1021735,1022450,1023165,1023880,1027461,1028178,1028895,1029612,1033203,1033922,1034641,
1035360,1038961,1039682,1040403,1041124,1044735,1045458,1046181,1046904,1050525,1051250,1051975,
1052700,1056331,1057058,1057785,1058512,1062153,1062882,1063611,1064340,1067991,1068722,1069453,
1070184,1073845,1074578,1075311,1076044,1079715,1080450,1081185,1081920,1085601,1086338,1087075,
1087812,1091503,1092242,1092981,1093720,1097421,1098162,1098903,1099644,1103355,1104098,1104841,
1105584,1109305,1110050,1110795,1111540,1115271,1116018,1116765,1117512,1121253,1122002,1122751,
1123500,1127251,1128002,1128753,1129504,1133265,1134018,1134771,1135524,1139295,1140050,1140805,
1141560,1145341,1146098,1146855,1147612,1151403,1152162,1152921,1153680,1157481,1158242,1159003,
1159764,1163575,1164338,1165101,1165864,1169685,1170450,1171215,1171980,1175811,1176578,1177345,
1178112,1181953,1182722,1183491,1184260,1188111,1188882,1189653,1190424,1194285,1195058,1195831,
1196604,1200475,1201250,1202025,1202800,1206681,1207458,1208235,1209012,1212903,1213682,1214461,
1215240,1219141,1219922,1220703,1221484,1225395,1226178,1226961,1227744,1231665,1232450,1233235,
1234020,1237951,1238738,1239525,1240312,1244253,1245042,1245831,1246620,1250571,1251362,1252153,
1252944,1256905,1257698,1258491,1259284,1263255,1264050,1264845,1265640,1269621,1270418,1271215,
1272012,1276003,1276802,1277601,1278400,1282401,1283202,1284003,1284804,1288815,1289618,1290421,
1291224,1295245,1296050,1296855,1297660,1301691,1302498,1303305,1304112,1308153,1308962,1309771,
1310580,1314631,1315442,1316253,1317064,1321125,1321938,1322751,1323564,1327635,1328450,1329265,
1330080,1334161,1334978,1335795,1336612,1340703,1341522,1342341,1343160,1347261,1348082,1348903,
1349724,1353835,1354658,1355481,1356304,1360425,1361250,1362075,1362900,1367031,1367858,1368685,
1369512,1373653,1374482,1375311,1376140,1380291,1381122,1381953,1382784,1386945,1387778,1388611,
1389444,1393615,1394450,1395285,1396120,1400301,1401138,1401975,1402812,1407003,1407842,1408681,
1409520,1413721,1414562,1415403,1416244,1420455,1421298,1422141,1422984,1427205,1428050,1428895,
1429740,1433971,1434818,1435665,1436512,1440753,1441602,1442451,1443300,1447551,1448402,1449253,
1450104,1454365,1455218,1456071,1456924,1461195,1462050,1462905,1463760,1468041,1468898,1469755,
1470612,1474903,1475762,1476621,1477480,1481781,1482642,1483503,1484364,1488675,1489538,1490401,
1491264,1495585,1496450,1497315,1498180,1502511,1503378,1504245,1505112,1509453,1510322,1511191,
1512060,1516411,1517282,1518153,1519024,1523385,1524258,1525131,1526004,1530375,1531250,1532125,
1533000,1537381,1538258,1539135,1540012,1544403,1545282,1546161,1547040,1551441,1552322,1553203,
1554084,1558495,1559378,1560261,1561144,1565565,1566450,1567335,1568220,1572651,1573538,1574425,
1575312,1579753,1580642,1581531,1582420,1586871,1587762,1588653,1589544,1594005,1594898,1595791,
1596684,1601155,1602050,1602945,1603840,1608321,1609218,1610115,1611012,1615503,1616402,1617301,
1618200,1622701,1623602,1624503,1625404,1629915,1630818,1631721,1632624,1637145,1638050,1638955,
1639860,1644391,1645298,1646205,1647112,1651653,1652562,1653471,1654380,1658931,1659842,1660753,
1661664,1666225,1667138,1668051,1668964,1673535,1674450,1675365,1676280,1680861,1681778,1682695,
1683612,1688203,1689122,1690041,1690960,1695561,1696482,1697403,1698324,1702935,1703858,1704781,
1705704,1710325,1711250,1712175,1713100,1717731,1718658,1719585,1720512,1725153,1726082,1727011,
1727940,1732591,1733522,1734453,1735384,1740045,1740978,1741911,1742844,1747515,1748450,1749385,
1750320,1755001,1755938,1756875,1757812,1762503,1763442,1764381,1765320,1770021,1770962,1771903,
1772844,1777555,1778498,1779441,1780384,1785105,1786050,1786995,1787940,1792671,1793618,1794565,
1795512,1800253,1801202,1802151,1803100,1807851,1808802,1809753,1810704,1815465,1816418,1817371,
1818324,1823095,1824050,1825005,1825960,1830741,1831698,1832655,1833612,1838403,1839362,1840321,
1841280,1846081,1847042,1848003,1848964,1853775,1854738,1855701,1856664,1861485,1862450,1863415,
1864380,1869211,1870178,1871145,1872112,1876953,1877922,1878891,1879860,1884711,1885682,1886653,
1887624,1892485,1893458,1894431,1895404,1900275,1901250,1902225,1903200,1908081,1909058,1910035,
1911012,1915903,1916882,1917861,1918840,1923741,1924722,1925703,1926684,1931595,1932578,1933561,
1934544,1939465,1940450,1941435,1942420,1947351,1948338,1949325,1950312,1955253,1956242,1957231,
1958220,1963171,1964162,1965153,1966144,1971105,1972098,1973091,1974084,1979055,1980050,1981045,
1982040,1987021,1988018,1989015,1990012,1995003,1996002,1997001,1998000,2003001,2004002,2005003,
2006004,2011015,2012018,2013021,2014024,2019045,2020050,2021055,2022060,2027091,2028098,2029105,
2030112,2035153,2036162,2037171,2038180,2043231,2044242,2045253,2046264,2051325,2052338,2053351,
2054364,2059435,2060450,2061465,2062480,2067561,2068578,2069595,2070612,2075703,2076722,2077741,
2078760,2083861,2084882,2085903,2086924,2092035,2093058,2094081,2095104,2100225,2101250,2102275,
2103300,2108431,2109458,2110485,2111512,2116653,2117682,2118711,2119740,2124891,2125922,2126953,
2127984,2133145,2134178,2135211,2136244,2141415,2142450,2143485,2144520,2149701,2150738,2151775,
2152812,2158003,2159042,2160081,2161120,2166321,2167362,2168403,2169444,2174655,2175698,2176741,
2177784,2183005,2184050,2185095,2186140,2191371,2192418,2193465,2194512,2199753,2200802,2201851,
2202900,2208151,2209202,2210253,2211304,2216565,2217618,2218671,2219724,2224995,2226050,2227105,
2228160,2233441,2234498,2235555,2236612,2241903,2242962,2244021,2245080,2250381,2251442,2252503,
2253564,2258875,2259938,2261001,2262064,2267385,2268450,2269515,2270580,2275911,2276978,2278045,
2279112,2284453,2285522,2286591,2287660,2293011,2294082,2295153,2296224,2301585,2302658,2303731,
2304804,2310175,2311250,2312325,2313400,2318781,2319858,2320935,2322012,2327403,2328482,2329561,
2330640,2336041,2337122,2338203,2339284,2344695,2345778,2346861,2347944,2353365,2354450,2355535,
2356620,2362051,2363138,2364225,2365312,2370753,2371842,2372931,2374020,2379471,2380562,2381653,
2382744,2388205,2389298,2390391,2391484,2396955,2398050,2399145,2400240,2405721,2406818,2407915,
2409012,2414503,2415602,2416701,2417800,2423301,2424402,2425503,2426604,2432115,2433218,2434321,
2435424,2440945,2442050,2443155,2444260,2449791,2450898,2452005,2453112,2458653,2459762,2460871,
2461980,2467531,2468642,2469753,2470864,2476425,2477538,2478651,2479764,2485335,2486450,2487565,
2488680,2494261,2495378,2496495,2497612,2503203,2504322,2505441,2506560,2512161,2513282,2514403,
2515524,2521135,2522258,2523381,2524504,2530125,2531250,2532375,2533500,2539131,2540258,2541385,
2542512,2548153,2549282,2550411,2551540,2557191,2558322,2559453,2560584,2566245,2567378,2568511,
2569644,2575315,2576450,2577585,2578720,2584401,2585538,2586675,2587812,2593503,2594642,2595781,
2596920,2602621,2603762,2604903,2606044,2611755,2612898,2614041,2615184,2620905,2622050,2623195,
2624340,2630071,2631218,2632365,2633512,2639253,2640402,2641551,2642700,2648451,2649602,2650753,
2651904,2657665,2658818,2659971,2661124,2666895,2668050,2669205,2670360,2676141,2677298,2678455,
2679612,2685403,2686562,2687721,2688880,2694681,2695842,2697003,2698164,2703975,2705138,2706301,
2707464,2713285,2714450,2715615,2716780,2722611,2723778,2724945,2726112,2731953,2733122,2734291,
2735460,2741311,2742482,2743653,2744824,2750685,2751858,2753031,2754204,2760075,2761250,2762425,
2763600,2769481,2770658,2771835,2773012,2778903,2780082,2781261,2782440,2788341,2789522,2790703,
2791884,2797795,2798978,2800161,2801344,2807265,2808450,2809635,2810820,2816751,2817938,2819125,
2820312,2826253,2827442,2828631,2829820,2835771,2836962,2838153,2839344,2845305,2846498,2847691,
2848884,2854855,2856050,2857245,2858440,2864421,2865618,2866815,2868012,2874003,2875202,2876401,
2877600,2883601,2884802,2886003,2887204,2893215,2894418,2895621,2896824,2902845,2904050,2905255,
2906460,2912491,2913698,2914905,2916112,2922153,2923362,2924571,2925780,2931831,2933042,2934253,
2935464,2941525,2942738,2943951,2945164,2951235,2952450,2953665,2954880,2960961,2962178,2963395,
2964612,2970703,2971922,2973141,2974360,2980461,2981682,2982903,2984124,2990235,2991458,2992681,
2993904,3000025,3001250,3002475,3003700,3009831,3011058,3012285,3013512,3019653,3020882,3022111,
3023340,3029491,3030722,3031953,3033184,3039345,3040578,3041811,3043044,3049215,3050450,3051685,
3052920,3059101,3060338,3061575,3062812,3069003,3070242,3071481,3072720,3078921,3080162,3081403,
3082644,3088855,3090098,3091341,3092584,3098805,3100050,3101295,3102540,3108771,3110018,3111265,
3112512,3118753,3120002,3121251,3122500,3128751,3130002,3131253,3132504,3138765,3140018,3141271,
3142524,3148795,3150050,3151305,3152560,3158841,3160098,3161355,3162612,3168903,3170162,3171421,
3172680,3178981,3180242,3181503,3182764,3189075,3190338,3191601,3192864,3199185,3200450,3201715,
3202980,3209311,3210578,3211845,3213112,3219453,3220722,3221991,3223260,3229611,3230882,3232153,
3233424,3239785,3241058,3242331,3243604,3249975,3251250,3252525,3253800,3260181,3261458,3262735,
3264012,3270403,3271682,3272961,3274240,3280641,3281922,3283203,3284484,3290895,3292178,3293461,
3294744,3301165,3302450,3303735,3305020,3311451,3312738,3314025,3315312,3321753,3323042,3324331,
3325620,3332071,3333362,3334653,3335944,3342405,3343698,3344991,3346284,3352755,3354050,3355345,
3356640,3363121,3364418,3365715,3367012,3373503,3374802,3376101,3377400,3383901,3385202,3386503,
3387804,3394315,3395618,3396921,3398224,3404745,3406050,3407355,3408660,3415191,3416498,3417805,
3419112,3425653,3426962,3428271,3429580,3436131,3437442,3438753,3440064,3446625,3447938,3449251,
3450564,3457135,3458450,3459765,3461080,3467661,3468978,3470295,3471612,3478203,3479522,3480841,
3482160,3488761,3490082,3491403,3492724,3499335,3500658,3501981,3503304,3509925,3511250,3512575,
3513900,3520531,3521858,3523185,3524512,3531153,3532482,3533811,3535140,3541791,3543122,3544453,
3545784,3552445,3553778,3555111,3556444,3563115,3564450,3565785,3567120,3573801,3575138,3576475,
3577812,3584503,3585842,3587181,3588520,3595221,3596562,3597903,3599244,3605955,3607298,3608641,
3609984,3616705,3618050,3619395,3620740,3627471,3628818,3630165,3631512,3638253,3639602,3640951,
3642300,3649051,3650402,3651753,3653104,3659865,3661218,3662571,3663924,3670695,3672050,3673405,
3674760,3681541,3682898,3684255,3685612,3692403,3693762,3695121,3696480,3703281,3704642,3706003,
3707364,3714175,3715538,3716901,3718264,3725085,3726450,3727815,3729180,3736011,3737378,3738745,
3740112,3746953,3748322,3749691,3751060,3757911,3759282,3760653,3762024,3768885,3770258,3771631,
3773004,3779875,3781250,3782625,3784000,3790881,3792258,3793635,3795012,3801903,3803282,3804661,
3806040,3812941,3814322,3815703,3817084,3823995,3825378,3826761,3828144,3835065,3836450,3837835,
3839220,3846151,3847538,3848925,3850312,3857253,3858642,3860031,3861420,3868371,3869762,3871153,
3872544,3879505,3880898,3882291,3883684,3890655,3892050,3893445,3894840,3901821,3903218,3904615,
3906012,3913003,3914402,3915801,3917200,3924201,3925602,3927003,3928404,3935415,3936818,3938221,
3939624,3946645,3948050,3949455,3950860,3957891,3959298,3960705,3962112,3969153,3970562,3971971,
3973380,3980431,3981842,3983253,3984664,3991725,3993138,3994551,3995964,4003035,4004450,4005865,
4007280,4014361,4015778,4017195,4018612,4025703,4027122,4028541,4029960,4037061,4038482,4039903,
4041324,4048435,4049858,4051281,4052704,4059825,4061250,4062675,4064100,4071231,4072658,4074085,
4075512,4082653,4084082,4085511,4086940,4094091,4095522,4096953,4098384,4105545,4106978,4108411,
4109844,4117015,4118450,4119885,4121320,4128501,4129938,4131375,4132812,4140003,4141442,4142881,
4144320,4151521,4152962,4154403,4155844,4163055,4164498,4165941,4167384,4174605,4176050,4177495,
4178940,4186171,4187618,4189065,4190512,4197753,4199202,4200651,4202100,4209351,4210802,4212253,
4213704,4220965,4222418,4223871,4225324,4232595,4234050,4235505,4236960,4244241,4245698,4247155,
4248612,4255903,4257362,4258821,4260280,4267581,4269042,4270503,4271964,4279275,4280738,4282201,
4283664,4290985,4292450,4293915,4295380,4302711,4304178,4305645,4307112,4314453,4315922,4317391,
4318860,4326211,4327682,4329153,4330624,4337985,4339458,4340931,4342404,4349775,4351250,4352725,
4354200,4361581,4363058,4364535,4366012,4373403,4374882,4376361,4377840,4385241,4386722,4388203,
4389684,4397095,4398578,4400061,4401544,4408965,4410450,4411935,4413420,4420851,4422338,4423825,
4425312,4432753,4434242,4435731,4437220,4444671,4446162,4447653,4449144,4456605,4458098,4459591,
4461084,4468555,4470050,4471545,4473040,4480521,4482018,4483515,4485012,4492503,4494002,4495501,
4497000,4504501,4506002,4507503,4509004,4516515,4518018,4519521,4521024,4528545,4530050,4531555,
4533060,4540591,4542098,4543605,4545112,4552653,4554162,4555671,4557180,4564731,4566242,4567753,
4569264,4576825,4578338,4579851,4581364,4588935,4590450,4591965,4593480,4601061,4602578,4604095,
4605612,4613203,4614722,4616241,4617760,4625361,4626882,4628403,4629924,4637535,4639058,4640581,
4642104,4649725,4651250,4652775,4654300,4661931,4663458,4664985,4666512,4674153,4675682,4677211,
4678740,4686391,4687922,4689453,4690984,4698645,4700178,4701711,4703244,4710915,4712450,4713985,
4715520,4723201,4724738,4726275,4727812,4735503,4737042,4738581,4740120,4747821,4749362,4750903,
4752444,4760155,4761698,4763241,4764784,4772505,4774050,4775595,4777140,4784871,4786418,4787965,
4789512,4797253,4798802,4800351,4801900,4809651,4811202,4812753,4814304,4822065,4823618,4825171,
4826724,4834495,4836050,4837605,4839160,4846941,4848498,4850055,4851612,4859403,4860962,4862521,
4864080,4871881,4873442,4875003,4876564,4884375,4885938,4887501,4889064,4896885,4898450,4900015,
4901580,4909411,4910978,4912545,4914112,4921953,4923522,4925091,4926660,4934511,4936082,4937653,
4939224,4947085,4948658,4950231,4951804,4959675,4961250,4962825,4964400,4972281,4973858,4975435,
4977012,4984903,4986482,4988061,4989640,4997541,4999122,5000703,5002284,5010195,5011778,5013361,
5014944,5022865,5024450,5026035,5027620,5035551,5037138,5038725,5040312,5048253,5049842,5051431,
5053020,5060971,5062562,5064153,5065744,5073705,5075298,5076891,5078484,5086455,5088050,5089645,
5091240,5099221,5100818,5102415,5104012,5112003,5113602,5115201,5116800,5124801,5126402,5128003,
5129604,5137615,5139218,5140821,5142424,5150445,5152050,5153655,5155260,5163291,5164898,5166505,
5168112,5176153,5177762,5179371,5180980,5189031,5190642,5192253,5193864,5201925,5203538,5205151,
5206764,5214835,5216450,5218065,5219680,5227761,5229378,5230995,5232612,5240703,5242322,5243941,
5245560,5253661,5255282,5256903,5258524,5266635,5268258,5269881,5271504,5279625,5281250,5282875,
5284500,5292631,5294258,5295885,5297512,5305653,5307282,5308911,5310540,5318691,5320322,5321953,
5323584,5331745,5333378,5335011,5336644,5344815,5346450,5348085,5349720,5357901,5359538,5361175,
5362812,5371003,5372642,5374281,5375920,5384121,5385762,5387403,5389044,5397255,5398898,5400541,
5402184,5410405,5412050,5413695,5415340,5423571,5425218,5426865,5428512,5436753,5438402,5440051,
5441700,5449951,5451602,5453253,5454904,5463165,5464818,5466471,5468124,5476395,5478050,5479705,
5481360,5489641,5491298,5492955,5494612,5502903,5504562,5506221,5507880,5516181,5517842,5519503,
5521164,5529475,5531138,5532801,5534464,5542785,5544450,5546115,5547780,5556111,5557778,5559445,
5561112,5569453,5571122,5572791,5574460,5582811,5584482,5586153,5587824,5596185,5597858,5599531,
5601204,5609575,5611250,5612925,5614600,5622981,5624658,5626335,5628012,5636403,5638082,5639761,
5641440,5649841,5651522,5653203,5654884,5663295,5664978,5666661,5668344,5676765,5678450,5680135,
5681820,5690251,5691938,5693625,5695312,5703753,5705442,5707131,5708820,5717271,5718962,5720653,
5722344,5730805,5732498,5734191,5735884,5744355,5746050,5747745,5749440,5757921,5759618,5761315,
5763012,5771503,5773202,5774901,5776600,5785101,5786802,5788503,5790204,5798715,5800418,5802121,
5803824,5812345,5814050,5815755,5817460,5825991,5827698,5829405,5831112,5839653,5841362,5843071,
5844780,5853331,5855042,5856753,5858464,5867025,5868738,5870451,5872164,5880735,5882450,5884165,
5885880,5894461,5896178,5897895,5899612,5908203,5909922,5911641,5913360,5921961,5923682,5925403,
5927124,5935735,5937458,5939181,5940904,5949525,5951250,5952975,5954700,5963331,5965058,5966785,
5968512,5977153,5978882,5980611,5982340,5990991,5992722,5994453,5996184,6004845,6006578,6008311,
6010044,6018715,6020450,6022185,6023920,6032601,6034338,6036075,6037812,6046503,6048242,6049981,
6051720,6060421,6062162,6063903,6065644,6074355,6076098,6077841,6079584,6088305,6090050,6091795,
6093540,6102271,6104018,6105765,6107512,6116253,6118002,6119751,6121500,6130251,6132002,6133753,
6135504,6144265,6146018,6147771,6149524,6158295,6160050,6161805,6163560,6172341,6174098,6175855,
6177612,6186403,6188162,6189921,6191680,6200481,6202242,6204003,6205764,6214575,6216338,6218101,
6219864,6228685,6230450,6232215,6233980,6242811,6244578,6246345,6248112,6256953,6258722,6260491,
6262260,6271111,6272882,6274653,6276424,6285285,6287058,6288831,6290604,6299475,6301250,6303025,
6304800,6313681,6315458,6317235,6319012,6327903,6329682,6331461,6333240,6342141,6343922,6345703,
6347484,6356395,6358178,6359961,6361744,6370665,6372450,6374235,6376020,6384951,6386738,6388525,
6390312,6399253,6401042,6402831,6404620,6413571,6415362,6417153,6418944,6427905,6429698,6431491,
6433284,6442255,6444050,6445845,6447640,6456621,6458418,6460215,6462012,6471003,6472802,6474601,
6476400,6485401,6487202,6489003,6490804,6499815,6501618,6503421,6505224,6514245,6516050,6517855,
6519660,6528691,6530498,6532305,6534112,6543153,6544962,6546771,6548580,6557631,6559442,6561253,
6563064,6572125,6573938,6575751,6577564,6586635,6588450,6590265,6592080,6601161,6602978,6604795,
6606612,6615703,6617522,6619341,6621160,6630261,6632082,6633903,6635724,6644835,6646658,6648481,
6650304,6659425,6661250,6663075,6664900,6674031,6675858,6677685,6679512,6688653,6690482,6692311,
6694140,6703291,6705122,6706953,6708784,6717945,6719778,6721611,6723444,6732615,6734450,6736285,
6738120,6747301,6749138,6750975,6752812,6762003,6763842,6765681,6767520,6776721,6778562,6780403,
6782244,6791455,6793298,6795141,6796984,6806205,6808050,6809895,6811740,6820971,6822818,6824665,
6826512,6835753,6837602,6839451,6841300,6850551,6852402,6854253,6856104,6865365,6867218,6869071,
6870924,6880195,6882050,6883905,6885760,6895041,6896898,6898755,6900612,6909903,6911762,6913621,
6915480,6924781,6926642,6928503,6930364,6939675,6941538,6943401,6945264,6954585,6956450,6958315,
6960180,6969511,6971378,6973245,6975112,6984453,6986322,6988191,6990060,6999411,7001282,7003153,
7005024,7014385,7016258,7018131,7020004,7029375,7031250,7033125,7035000,7044381,7046258,7048135,
7050012,7059403,7061282,7063161,7065040,7074441,7076322,7078203,7080084,7089495,7091378,7093261,
7095144,7104565,7106450,7108335,7110220,7119651,7121538,7123425,7125312,7134753,7136642,7138531,
7140420,7149871,7151762,7153653,7155544,7165005,7166898,7168791,7170684,7180155,7182050,7183945,
7185840,7195321,7197218,7199115,7201012,7210503,7212402,7214301,7216200,7225701,7227602,7229503,
7231404,7240915,7242818,7244721,7246624,7256145,7258050,7259955,7261860,7271391,7273298,7275205,
7277112,7286653,7288562,7290471,7292380,7301931,7303842,7305753,7307664,7317225,7319138,7321051,
7322964,7332535,7334450,7336365,7338280,7347861,7349778,7351695,7353612,7363203,7365122,7367041,
7368960,7378561,7380482,7382403,7384324,7393935,7395858,7397781,7399704,7409325,7411250,7413175,
7415100,7424731,7426658,7428585,7430512,7440153,7442082,7444011,7445940,7455591,7457522,7459453,
7461384,7471045,7472978,7474911,7476844,7486515,7488450,7490385,7492320,7502001,7503938,7505875,
7507812,7517503,7519442,7521381,7523320,7533021,7534962,7536903,7538844,7548555,7550498,7552441,
7554384,7564105,7566050,7567995,7569940,7579671,7581618,7583565,7585512,7595253,7597202,7599151,
7601100,7610851,7612802,7614753,7616704,7626465,7628418,7630371,7632324,7642095,7644050,7646005,
7647960,7657741,7659698,7661655,7663612,7673403,7675362,7677321,7679280,7689081,7691042,7693003,
7694964,7704775,7706738,7708701,7710664,7720485,7722450,7724415,7726380,7736211,7738178,7740145,
7742112,7751953,7753922,7755891,7757860,7767711,7769682,7771653,7773624,7783485,7785458,7787431,
7789404,7799275,7801250,7803225,7805200,7815081,7817058,7819035,7821012,7830903,7832882,7834861,
7836840,7846741,7848722,7850703,7852684,7862595,7864578,7866561,7868544,7878465,7880450,7882435,
7884420,7894351,7896338,7898325,7900312,7910253,7912242,7914231,7916220,7926171,7928162,7930153,
7932144,7942105,7944098,7946091,7948084,7958055,7960050,7962045,7964040,7974021,7976018,7978015,
7980012,7990003,7992002,7994001,7996000,8006001,8008002,8010003,8012004,8022015,8024018,8026021,
8028024,8038045,8040050,8042055,8044060,8054091,8056098,8058105,8060112,8070153,8072162,8074171,
8076180,8086231,8088242,8090253,8092264,8102325,8104338,8106351,8108364,8118435,8120450,8122465,
8124480,8134561,8136578,8138595,8140612,8150703,8152722,8154741,8156760,8166861,8168882,8170903,
8172924,8183035,8185058,8187081,8189104,8199225,8201250,8203275,8205300,8215431,8217458,8219485,
8221512,8231653,8233682,8235711,8237740,8247891,8249922,8251953,8253984,8264145,8266178,8268211,
8270244,8280415,8282450,8284485,8286520,8296701,8298738,8300775,8302812,8313003,8315042,8317081,
8319120,8329321,8331362,8333403,8335444,8345655,8347698,8349741,8351784,8362005,8364050,8366095,
8368140,8378371,8380418,8382465,8384512,
};
static const uint32_t mdkit_pi_sd_word7_delta[4097]={
0,0,0,0,1,1,1,1,2,2,2,2,
3,3,3,3,4,4,4,4,5,5,5,5,
6,6,6,6,7,7,7,7,8,8,8,8,
9,9,9,9,10,10,10,10,11,11,11,11,
12,12,12,12,13,13,13,13,14,14,14,14,
15,15,15,15,16,16,16,16,17,17,17,17,
18,18,18,18,19,19,19,19,20,20,20,20,
21,21,21,21,22,22,22,22,23,23,23,23,
24,24,24,24,25,25,25,25,26,26,26,26,
27,27,27,27,28,28,28,28,29,29,29,29,
30,30,30,30,31,31,31,31,32,32,32,32,
33,33,33,33,34,34,34,34,35,35,35,35,
36,36,36,36,37,37,37,37,38,38,38,38,
39,39,39,39,40,40,40,40,41,41,41,41,
42,42,42,42,43,43,43,43,44,44,44,44,
45,45,45,45,46,46,46,46,47,47,47,47,
48,48,48,48,49,49,49,49,50,50,50,50,
51,51,51,51,52,52,52,52,53,53,53,53,
54,54,54,54,55,55,55,55,56,56,56,56,
57,57,57,57,58,58,58,58,59,59,59,59,
60,60,60,60,61,61,61,61,62,62,62,62,
63,63,63,63,64,64,64,64,65,65,65,65,
66,66,66,66,67,67,67,67,68,68,68,68,
69,69,69,69,70,70,70,70,71,71,71,71,
72,72,72,72,73,73,73,73,74,74,74,74,
75,75,75,75,76,76,76,76,77,77,77,77,
78,78,78,78,79,79,79,79,80,80,80,80,
81,81,81,81,82,82,82,82,83,83,83,83,
84,84,84,84,85,85,85,85,86,86,86,86,
87,87,87,87,88,88,88,88,89,89,89,89,
90,90,90,90,91,91,91,91,92,92,92,92,
93,93,93,93,94,94,94,94,95,95,95,95,
96,96,96,96,97,97,97,97,98,98,98,98,
99,99,99,99,100,100,100,100,101,101,101,101,
102,102,102,102,103,103,103,103,104,104,104,104,
105,105,105,105,106,106,106,106,107,107,107,107,
108,108,108,108,109,109,109,109,110,110,110,110,
111,111,111,111,112,112,112,112,113,113,113,113,
114,114,114,114,115,115,115,115,116,116,116,116,
117,117,117,117,118,118,118,118,119,119,119,119,
120,120,120,120,121,121,121,121,122,122,122,122,
123,123,123,123,124,124,124,124,125,125,125,125,
126,126,126,126,127,127,127,127,128,128,128,128,
129,129,129,129,130,130,130,130,131,131,131,131,
132,132,132,132,133,133,133,133,134,134,134,134,
135,135,135,135,136,136,136,136,137,137,137,137,
138,138,138,138,139,139,139,139,140,140,140,140,
141,141,141,141,142,142,142,142,143,143,143,143,
144,144,144,144,145,145,145,145,146,146,146,146,
147,147,147,147,148,148,148,148,149,149,149,149,
150,150,150,150,151,151,151,151,152,152,152,152,
153,153,153,153,154,154,154,154,155,155,155,155,
156,156,156,156,157,157,157,157,158,158,158,158,
159,159,159,159,160,160,160,160,161,161,161,161,
162,162,162,162,163,163,163,163,164,164,164,164,
165,165,165,165,166,166,166,166,167,167,167,167,
168,168,168,168,169,169,169,169,170,170,170,170,
171,171,171,171,172,172,172,172,173,173,173,173,
174,174,174,174,175,175,175,175,176,176,176,176,
177,177,177,177,178,178,178,178,179,179,179,179,
180,180,180,180,181,181,181,181,182,182,182,182,
183,183,183,183,184,184,184,184,185,185,185,185,
186,186,186,186,187,187,187,187,188,188,188,188,
189,189,189,189,190,190,190,190,191,191,191,191,
192,192,192,192,193,193,193,193,194,194,194,194,
195,195,195,195,196,196,196,196,197,197,197,197,
198,198,198,198,199,199,199,199,200,200,200,200,
201,201,201,201,202,202,202,202,203,203,203,203,
204,204,204,204,205,205,205,205,206,206,206,206,
207,207,207,207,208,208,208,208,209,209,209,209,
210,210,210,210,211,211,211,211,212,212,212,212,
213,213,213,213,214,214,214,214,215,215,215,215,
216,216,216,216,217,217,217,217,218,218,218,218,
219,219,219,219,220,220,220,220,221,221,221,221,
222,222,222,222,223,223,223,223,224,224,224,224,
225,225,225,225,226,226,226,226,227,227,227,227,
228,228,228,228,229,229,229,229,230,230,230,230,
231,231,231,231,232,232,232,232,233,233,233,233,
234,234,234,234,235,235,235,235,236,236,236,236,
237,237,237,237,238,238,238,238,239,239,239,239,
240,240,240,240,241,241,241,241,242,242,242,242,
243,243,243,243,244,244,244,244,245,245,245,245,
246,246,246,246,247,247,247,247,248,248,248,248,
249,249,249,249,250,250,250,250,251,251,251,251,
252,252,252,252,253,253,253,253,254,254,254,254,
255,255,255,255,256,256,256,256,257,257,257,257,
258,258,258,258,259,259,259,259,260,260,260,260,
261,261,261,261,262,262,262,262,263,263,263,263,
264,264,264,264,265,265,265,265,266,266,266,266,
267,267,267,267,268,268,268,268,269,269,269,269,
270,270,270,270,271,271,271,271,272,272,272,272,
273,273,273,273,274,274,274,274,275,275,275,275,
276,276,276,276,277,277,277,277,278,278,278,278,
279,279,279,279,280,280,280,280,281,281,281,281,
282,282,282,282,283,283,283,283,284,284,284,284,
285,285,285,285,286,286,286,286,287,287,287,287,
288,288,288,288,289,289,289,289,290,290,290,290,
291,291,291,291,292,292,292,292,293,293,293,293,
294,294,294,294,295,295,295,295,296,296,296,296,
297,297,297,297,298,298,298,298,299,299,299,299,
300,300,300,300,301,301,301,301,302,302,302,302,
303,303,303,303,304,304,304,304,305,305,305,305,
306,306,306,306,307,307,307,307,308,308,308,308,
309,309,309,309,310,310,310,310,311,311,311,311,
312,312,312,312,313,313,313,313,314,314,314,314,
315,315,315,315,316,316,316,316,317,317,317,317,
318,318,318,318,319,319,319,319,320,320,320,320,
321,321,321,321,322,322,322,322,323,323,323,323,
324,324,324,324,325,325,325,325,326,326,326,326,
327,327,327,327,328,328,328,328,329,329,329,329,
330,330,330,330,331,331,331,331,332,332,332,332,
333,333,333,333,334,334,334,334,335,335,335,335,
336,336,336,336,337,337,337,337,338,338,338,338,
339,339,339,339,340,340,340,340,341,341,341,341,
342,342,342,342,343,343,343,343,344,344,344,344,
345,345,345,345,346,346,346,346,347,347,347,347,
348,348,348,348,349,349,349,349,350,350,350,350,
351,351,351,351,352,352,352,352,353,353,353,353,
354,354,354,354,355,355,355,355,356,356,356,356,
357,357,357,357,358,358,358,358,359,359,359,359,
360,360,360,360,361,361,361,361,362,362,362,362,
363,363,363,363,364,364,364,364,365,365,365,365,
366,366,366,366,367,367,367,367,368,368,368,368,
369,369,369,369,370,370,370,370,371,371,371,371,
372,372,372,372,373,373,373,373,374,374,374,374,
375,375,375,375,376,376,376,376,377,377,377,377,
378,378,378,378,379,379,379,379,380,380,380,380,
381,381,381,381,382,382,382,382,383,383,383,383,
384,384,384,384,385,385,385,385,386,386,386,386,
387,387,387,387,388,388,388,388,389,389,389,389,
390,390,390,390,391,391,391,391,392,392,392,392,
393,393,393,393,394,394,394,394,395,395,395,395,
396,396,396,396,397,397,397,397,398,398,398,398,
399,399,399,399,400,400,400,400,401,401,401,401,
402,402,402,402,403,403,403,403,404,404,404,404,
405,405,405,405,406,406,406,406,407,407,407,407,
408,408,408,408,409,409,409,409,410,410,410,410,
411,411,411,411,412,412,412,412,413,413,413,413,
414,414,414,414,415,415,415,415,416,416,416,416,
417,417,417,417,418,418,418,418,419,419,419,419,
420,420,420,420,421,421,421,421,422,422,422,422,
423,423,423,423,424,424,424,424,425,425,425,425,
426,426,426,426,427,427,427,427,428,428,428,428,
429,429,429,429,430,430,430,430,431,431,431,431,
432,432,432,432,433,433,433,433,434,434,434,434,
435,435,435,435,436,436,436,436,437,437,437,437,
438,438,438,438,439,439,439,439,440,440,440,440,
441,441,441,441,442,442,442,442,443,443,443,443,
444,444,444,444,445,445,445,445,446,446,446,446,
447,447,447,447,448,448,448,448,449,449,449,449,
450,450,450,450,451,451,451,451,452,452,452,452,
453,453,453,453,454,454,454,454,455,455,455,455,
456,456,456,456,457,457,457,457,458,458,458,458,
459,459,459,459,460,460,460,460,461,461,461,461,
462,462,462,462,463,463,463,463,464,464,464,464,
465,465,465,465,466,466,466,466,467,467,467,467,
468,468,468,468,469,469,469,469,470,470,470,470,
471,471,471,471,472,472,472,472,473,473,473,473,
474,474,474,474,475,475,475,475,476,476,476,476,
477,477,477,477,478,478,478,478,479,479,479,479,
480,480,480,480,481,481,481,481,482,482,482,482,
483,483,483,483,484,484,484,484,485,485,485,485,
486,486,486,486,487,487,487,487,488,488,488,488,
489,489,489,489,490,490,490,490,491,491,491,491,
492,492,492,492,493,493,493,493,494,494,494,494,
495,495,495,495,496,496,496,496,497,497,497,497,
498,498,498,498,499,499,499,499,500,500,500,500,
501,501,501,501,502,502,502,502,503,503,503,503,
504,504,504,504,505,505,505,505,506,506,506,506,
507,507,507,507,508,508,508,508,509,509,509,509,
510,510,510,510,511,511,511,511,512,512,512,512,
513,513,513,513,514,514,514,514,515,515,515,515,
516,516,516,516,517,517,517,517,518,518,518,518,
519,519,519,519,520,520,520,520,521,521,521,521,
522,522,522,522,523,523,523,523,524,524,524,524,
525,525,525,525,526,526,526,526,527,527,527,527,
528,528,528,528,529,529,529,529,530,530,530,530,
531,531,531,531,532,532,532,532,533,533,533,533,
534,534,534,534,535,535,535,535,536,536,536,536,
537,537,537,537,538,538,538,538,539,539,539,539,
540,540,540,540,541,541,541,541,542,542,542,542,
543,543,543,543,544,544,544,544,545,545,545,545,
546,546,546,546,547,547,547,547,548,548,548,548,
549,549,549,549,550,550,550,550,551,551,551,551,
552,552,552,552,553,553,553,553,554,554,554,554,
555,555,555,555,556,556,556,556,557,557,557,557,
558,558,558,558,559,559,559,559,560,560,560,560,
561,561,561,561,562,562,562,562,563,563,563,563,
564,564,564,564,565,565,565,565,566,566,566,566,
567,567,567,567,568,568,568,568,569,569,569,569,
570,570,570,570,571,571,571,571,572,572,572,572,
573,573,573,573,574,574,574,574,575,575,575,575,
576,576,576,576,577,577,577,577,578,578,578,578,
579,579,579,579,580,580,580,580,581,581,581,581,
582,582,582,582,583,583,583,583,584,584,584,584,
585,585,585,585,586,586,586,586,587,587,587,587,
588,588,588,588,589,589,589,589,590,590,590,590,
591,591,591,591,592,592,592,592,593,593,593,593,
594,594,594,594,595,595,595,595,596,596,596,596,
597,597,597,597,598,598,598,598,599,599,599,599,
600,600,600,600,601,601,601,601,602,602,602,602,
603,603,603,603,604,604,604,604,605,605,605,605,
606,606,606,606,607,607,607,607,608,608,608,608,
609,609,609,609,610,610,610,610,611,611,611,611,
612,612,612,612,613,613,613,613,614,614,614,614,
615,615,615,615,616,616,616,616,617,617,617,617,
618,618,618,618,619,619,619,619,620,620,620,620,
621,621,621,621,622,622,622,622,623,623,623,623,
624,624,624,624,625,625,625,625,626,626,626,626,
627,627,627,627,628,628,628,628,629,629,629,629,
630,630,630,630,631,631,631,631,632,632,632,632,
633,633,633,633,634,634,634,634,635,635,635,635,
636,636,636,636,637,637,637,637,638,638,638,638,
639,639,639,639,640,640,640,640,641,641,641,641,
642,642,642,642,643,643,643,643,644,644,644,644,
645,645,645,645,646,646,646,646,647,647,647,647,
648,648,648,648,649,649,649,649,650,650,650,650,
651,651,651,651,652,652,652,652,653,653,653,653,
654,654,654,654,655,655,655,655,656,656,656,656,
657,657,657,657,658,658,658,658,659,659,659,659,
660,660,660,660,661,661,661,661,662,662,662,662,
663,663,663,663,664,664,664,664,665,665,665,665,
666,666,666,666,667,667,667,667,668,668,668,668,
669,669,669,669,670,670,670,670,671,671,671,671,
672,672,672,672,673,673,673,673,674,674,674,674,
675,675,675,675,676,676,676,676,677,677,677,677,
678,678,678,678,679,679,679,679,680,680,680,680,
681,681,681,681,682,682,682,682,683,683,683,683,
684,684,684,684,685,685,685,685,686,686,686,686,
687,687,687,687,688,688,688,688,689,689,689,689,
690,690,690,690,691,691,691,691,692,692,692,692,
693,693,693,693,694,694,694,694,695,695,695,695,
696,696,696,696,697,697,697,697,698,698,698,698,
699,699,699,699,700,700,700,700,701,701,701,701,
702,702,702,702,703,703,703,703,704,704,704,704,
705,705,705,705,706,706,706,706,707,707,707,707,
708,708,708,708,709,709,709,709,710,710,710,710,
711,711,711,711,712,712,712,712,713,713,713,713,
714,714,714,714,715,715,715,715,716,716,716,716,
717,717,717,717,718,718,718,718,719,719,719,719,
720,720,720,720,721,721,721,721,722,722,722,722,
723,723,723,723,724,724,724,724,725,725,725,725,
726,726,726,726,727,727,727,727,728,728,728,728,
729,729,729,729,730,730,730,730,731,731,731,731,
732,732,732,732,733,733,733,733,734,734,734,734,
735,735,735,735,736,736,736,736,737,737,737,737,
738,738,738,738,739,739,739,739,740,740,740,740,
741,741,741,741,742,742,742,742,743,743,743,743,
744,744,744,744,745,745,745,745,746,746,746,746,
747,747,747,747,748,748,748,748,749,749,749,749,
750,750,750,750,751,751,751,751,752,752,752,752,
753,753,753,753,754,754,754,754,755,755,755,755,
756,756,756,756,757,757,757,757,758,758,758,758,
759,759,759,759,760,760,760,760,761,761,761,761,
762,762,762,762,763,763,763,763,764,764,764,764,
765,765,765,765,766,766,766,766,767,767,767,767,
768,768,768,768,769,769,769,769,770,770,770,770,
771,771,771,771,772,772,772,772,773,773,773,773,
774,774,774,774,775,775,775,775,776,776,776,776,
777,777,777,777,778,778,778,778,779,779,779,779,
780,780,780,780,781,781,781,781,782,782,782,782,
783,783,783,783,784,784,784,784,785,785,785,785,
786,786,786,786,787,787,787,787,788,788,788,788,
789,789,789,789,790,790,790,790,791,791,791,791,
792,792,792,792,793,793,793,793,794,794,794,794,
795,795,795,795,796,796,796,796,797,797,797,797,
798,798,798,798,799,799,799,799,800,800,800,800,
801,801,801,801,802,802,802,802,803,803,803,803,
804,804,804,804,805,805,805,805,806,806,806,806,
807,807,807,807,808,808,808,808,809,809,809,809,
810,810,810,810,811,811,811,811,812,812,812,812,
813,813,813,813,814,814,814,814,815,815,815,815,
816,816,816,816,817,817,817,817,818,818,818,818,
819,819,819,819,820,820,820,820,821,821,821,821,
822,822,822,822,823,823,823,823,824,824,824,824,
825,825,825,825,826,826,826,826,827,827,827,827,
828,828,828,828,829,829,829,829,830,830,830,830,
831,831,831,831,832,832,832,832,833,833,833,833,
834,834,834,834,835,835,835,835,836,836,836,836,
837,837,837,837,838,838,838,838,839,839,839,839,
840,840,840,840,841,841,841,841,842,842,842,842,
843,843,843,843,844,844,844,844,845,845,845,845,
846,846,846,846,847,847,847,847,848,848,848,848,
849,849,849,849,850,850,850,850,851,851,851,851,
852,852,852,852,853,853,853,853,854,854,854,854,
855,855,855,855,856,856,856,856,857,857,857,857,
858,858,858,858,859,859,859,859,860,860,860,860,
861,861,861,861,862,862,862,862,863,863,863,863,
864,864,864,864,865,865,865,865,866,866,866,866,
867,867,867,867,868,868,868,868,869,869,869,869,
870,870,870,870,871,871,871,871,872,872,872,872,
873,873,873,873,874,874,874,874,875,875,875,875,
876,876,876,876,877,877,877,877,878,878,878,878,
879,879,879,879,880,880,880,880,881,881,881,881,
882,882,882,882,883,883,883,883,884,884,884,884,
885,885,885,885,886,886,886,886,887,887,887,887,
888,888,888,888,889,889,889,889,890,890,890,890,
891,891,891,891,892,892,892,892,893,893,893,893,
894,894,894,894,895,895,895,895,896,896,896,896,
897,897,897,897,898,898,898,898,899,899,899,899,
900,900,900,900,901,901,901,901,902,902,902,902,
903,903,903,903,904,904,904,904,905,905,905,905,
906,906,906,906,907,907,907,907,908,908,908,908,
909,909,909,909,910,910,910,910,911,911,911,911,
912,912,912,912,913,913,913,913,914,914,914,914,
915,915,915,915,916,916,916,916,917,917,917,917,
918,918,918,918,919,919,919,919,920,920,920,920,
921,921,921,921,922,922,922,922,923,923,923,923,
924,924,924,924,925,925,925,925,926,926,926,926,
927,927,927,927,928,928,928,928,929,929,929,929,
930,930,930,930,931,931,931,931,932,932,932,932,
933,933,933,933,934,934,934,934,935,935,935,935,
936,936,936,936,937,937,937,937,938,938,938,938,
939,939,939,939,940,940,940,940,941,941,941,941,
942,942,942,942,943,943,943,943,944,944,944,944,
945,945,945,945,946,946,946,946,947,947,947,947,
948,948,948,948,949,949,949,949,950,950,950,950,
951,951,951,951,952,952,952,952,953,953,953,953,
954,954,954,954,955,955,955,955,956,956,956,956,
957,957,957,957,958,958,958,958,959,959,959,959,
960,960,960,960,961,961,961,961,962,962,962,962,
963,963,963,963,964,964,964,964,965,965,965,965,
966,966,966,966,967,967,967,967,968,968,968,968,
969,969,969,969,970,970,970,970,971,971,971,971,
972,972,972,972,973,973,973,973,974,974,974,974,
975,975,975,975,976,976,976,976,977,977,977,977,
978,978,978,978,979,979,979,979,980,980,980,980,
981,981,981,981,982,982,982,982,983,983,983,983,
984,984,984,984,985,985,985,985,986,986,986,986,
987,987,987,987,988,988,988,988,989,989,989,989,
990,990,990,990,991,991,991,991,992,992,992,992,
993,993,993,993,994,994,994,994,995,995,995,995,
996,996,996,996,997,997,997,997,998,998,998,998,
999,999,999,999,1000,1000,1000,1000,1001,1001,1001,1001,
1002,1002,1002,1002,1003,1003,1003,1003,1004,1004,1004,1004,
1005,1005,1005,1005,1006,1006,1006,1006,1007,1007,1007,1007,
1008,1008,1008,1008,1009,1009,1009,1009,1010,1010,1010,1010,
1011,1011,1011,1011,1012,1012,1012,1012,1013,1013,1013,1013,
1014,1014,1014,1014,1015,1015,1015,1015,1016,1016,1016,1016,
1017,1017,1017,1017,1018,1018,1018,1018,1019,1019,1019,1019,
1020,1020,1020,1020,1021,1021,1021,1021,1022,1022,1022,1022,
1023,1023,1023,1023,1024,
};
static uint32_t mdkit_pi_sd_word7(unsigned raw){
 unsigned low=0,high=4097;
 while(low+1<high){unsigned mid=(low+high)/2;if(mdkit_pi_sd_word7_start[mid]<=raw)low=mid;else high=mid;}
 return (mdkit_pi_sd_word7_value[low]+mdkit_pi_sd_word7_delta[low]*(raw-mdkit_pi_sd_word7_start[low]))&0xffffff;
}
static const uint16_t mdkit_pi_sd_word4_start[1]={
0,
};
static const uint32_t mdkit_pi_sd_word4_value[1]={
0,
};
static const uint32_t mdkit_pi_sd_word4_delta[1]={
32,
};
static uint32_t mdkit_pi_sd_word4(unsigned raw){
 unsigned low=0,high=1;
 while(low+1<high){unsigned mid=(low+high)/2;if(mdkit_pi_sd_word4_start[mid]<=raw)low=mid;else high=mid;}
 return (mdkit_pi_sd_word4_value[low]+mdkit_pi_sd_word4_delta[low]*(raw-mdkit_pi_sd_word4_start[low]))&0xffffff;
}
static const uint16_t mdkit_pi_sd_word5_start[4097]={
0,6,8,10,12,22,24,26,28,38,40,42,
44,54,56,58,60,70,72,74,76,86,88,90,
92,102,104,106,108,118,120,122,124,134,136,138,
140,150,152,154,156,166,168,170,172,182,184,186,
188,198,200,202,204,214,216,218,220,230,232,234,
236,246,248,250,252,262,264,266,268,278,280,282,
284,294,296,298,300,310,312,314,316,326,328,330,
332,342,344,346,348,358,360,362,364,374,376,378,
380,390,392,394,396,406,408,410,412,422,424,426,
428,438,440,442,444,454,456,458,460,470,472,474,
476,486,488,490,492,502,504,506,508,518,520,522,
524,534,536,538,540,550,552,554,556,566,568,570,
572,582,584,586,588,598,600,602,604,614,616,618,
620,630,632,634,636,646,648,650,652,662,664,666,
668,678,680,682,684,694,696,698,700,710,712,714,
716,726,728,730,732,742,744,746,748,758,760,762,
764,774,776,778,780,790,792,794,796,806,808,810,
812,822,824,826,828,838,840,842,844,854,856,858,
860,870,872,874,876,886,888,890,892,902,904,906,
908,918,920,922,924,934,936,938,940,950,952,954,
956,966,968,970,972,982,984,986,988,998,1000,1002,
1004,1014,1016,1018,1020,1030,1032,1034,1036,1046,1048,1050,
1052,1062,1064,1066,1068,1078,1080,1082,1084,1094,1096,1098,
1100,1110,1112,1114,1116,1126,1128,1130,1132,1142,1144,1146,
1148,1158,1160,1162,1164,1174,1176,1178,1180,1190,1192,1194,
1196,1206,1208,1210,1212,1222,1224,1226,1228,1238,1240,1242,
1244,1254,1256,1258,1260,1270,1272,1274,1276,1286,1288,1290,
1292,1302,1304,1306,1308,1318,1320,1322,1324,1334,1336,1338,
1340,1350,1352,1354,1356,1366,1368,1370,1372,1382,1384,1386,
1388,1398,1400,1402,1404,1414,1416,1418,1420,1430,1432,1434,
1436,1446,1448,1450,1452,1462,1464,1466,1468,1478,1480,1482,
1484,1494,1496,1498,1500,1510,1512,1514,1516,1526,1528,1530,
1532,1542,1544,1546,1548,1558,1560,1562,1564,1574,1576,1578,
1580,1590,1592,1594,1596,1606,1608,1610,1612,1622,1624,1626,
1628,1638,1640,1642,1644,1654,1656,1658,1660,1670,1672,1674,
1676,1686,1688,1690,1692,1702,1704,1706,1708,1718,1720,1722,
1724,1734,1736,1738,1740,1750,1752,1754,1756,1766,1768,1770,
1772,1782,1784,1786,1788,1798,1800,1802,1804,1814,1816,1818,
1820,1830,1832,1834,1836,1846,1848,1850,1852,1862,1864,1866,
1868,1878,1880,1882,1884,1894,1896,1898,1900,1910,1912,1914,
1916,1926,1928,1930,1932,1942,1944,1946,1948,1958,1960,1962,
1964,1974,1976,1978,1980,1990,1992,1994,1996,2006,2008,2010,
2012,2022,2024,2026,2028,2038,2040,2042,2044,2054,2056,2058,
2060,2070,2072,2074,2076,2086,2088,2090,2092,2102,2104,2106,
2108,2118,2120,2122,2124,2134,2136,2138,2140,2150,2152,2154,
2156,2166,2168,2170,2172,2182,2184,2186,2188,2198,2200,2202,
2204,2214,2216,2218,2220,2230,2232,2234,2236,2246,2248,2250,
2252,2262,2264,2266,2268,2278,2280,2282,2284,2294,2296,2298,
2300,2310,2312,2314,2316,2326,2328,2330,2332,2342,2344,2346,
2348,2358,2360,2362,2364,2374,2376,2378,2380,2390,2392,2394,
2396,2406,2408,2410,2412,2422,2424,2426,2428,2438,2440,2442,
2444,2454,2456,2458,2460,2470,2472,2474,2476,2486,2488,2490,
2492,2502,2504,2506,2508,2518,2520,2522,2524,2534,2536,2538,
2540,2550,2552,2554,2556,2566,2568,2570,2572,2582,2584,2586,
2588,2598,2600,2602,2604,2614,2616,2618,2620,2630,2632,2634,
2636,2646,2648,2650,2652,2662,2664,2666,2668,2678,2680,2682,
2684,2694,2696,2698,2700,2710,2712,2714,2716,2726,2728,2730,
2732,2742,2744,2746,2748,2758,2760,2762,2764,2774,2776,2778,
2780,2790,2792,2794,2796,2806,2808,2810,2812,2822,2824,2826,
2828,2838,2840,2842,2844,2854,2856,2858,2860,2870,2872,2874,
2876,2886,2888,2890,2892,2902,2904,2906,2908,2918,2920,2922,
2924,2934,2936,2938,2940,2950,2952,2954,2956,2966,2968,2970,
2972,2982,2984,2986,2988,2998,3000,3002,3004,3014,3016,3018,
3020,3030,3032,3034,3036,3046,3048,3050,3052,3062,3064,3066,
3068,3078,3080,3082,3084,3094,3096,3098,3100,3110,3112,3114,
3116,3126,3128,3130,3132,3142,3144,3146,3148,3158,3160,3162,
3164,3174,3176,3178,3180,3190,3192,3194,3196,3206,3208,3210,
3212,3222,3224,3226,3228,3238,3240,3242,3244,3254,3256,3258,
3260,3270,3272,3274,3276,3286,3288,3290,3292,3302,3304,3306,
3308,3318,3320,3322,3324,3334,3336,3338,3340,3350,3352,3354,
3356,3366,3368,3370,3372,3382,3384,3386,3388,3398,3400,3402,
3404,3414,3416,3418,3420,3430,3432,3434,3436,3446,3448,3450,
3452,3462,3464,3466,3468,3478,3480,3482,3484,3494,3496,3498,
3500,3510,3512,3514,3516,3526,3528,3530,3532,3542,3544,3546,
3548,3558,3560,3562,3564,3574,3576,3578,3580,3590,3592,3594,
3596,3606,3608,3610,3612,3622,3624,3626,3628,3638,3640,3642,
3644,3654,3656,3658,3660,3670,3672,3674,3676,3686,3688,3690,
3692,3702,3704,3706,3708,3718,3720,3722,3724,3734,3736,3738,
3740,3750,3752,3754,3756,3766,3768,3770,3772,3782,3784,3786,
3788,3798,3800,3802,3804,3814,3816,3818,3820,3830,3832,3834,
3836,3846,3848,3850,3852,3862,3864,3866,3868,3878,3880,3882,
3884,3894,3896,3898,3900,3910,3912,3914,3916,3926,3928,3930,
3932,3942,3944,3946,3948,3958,3960,3962,3964,3974,3976,3978,
3980,3990,3992,3994,3996,4006,4008,4010,4012,4022,4024,4026,
4028,4038,4040,4042,4044,4054,4056,4058,4060,4070,4072,4074,
4076,4086,4088,4090,4092,4102,4104,4106,4108,4118,4120,4122,
4124,4134,4136,4138,4140,4150,4152,4154,4156,4166,4168,4170,
4172,4182,4184,4186,4188,4198,4200,4202,4204,4214,4216,4218,
4220,4230,4232,4234,4236,4246,4248,4250,4252,4262,4264,4266,
4268,4278,4280,4282,4284,4294,4296,4298,4300,4310,4312,4314,
4316,4326,4328,4330,4332,4342,4344,4346,4348,4358,4360,4362,
4364,4374,4376,4378,4380,4390,4392,4394,4396,4406,4408,4410,
4412,4422,4424,4426,4428,4438,4440,4442,4444,4454,4456,4458,
4460,4470,4472,4474,4476,4486,4488,4490,4492,4502,4504,4506,
4508,4518,4520,4522,4524,4534,4536,4538,4540,4550,4552,4554,
4556,4566,4568,4570,4572,4582,4584,4586,4588,4598,4600,4602,
4604,4614,4616,4618,4620,4630,4632,4634,4636,4646,4648,4650,
4652,4662,4664,4666,4668,4678,4680,4682,4684,4694,4696,4698,
4700,4710,4712,4714,4716,4726,4728,4730,4732,4742,4744,4746,
4748,4758,4760,4762,4764,4774,4776,4778,4780,4790,4792,4794,
4796,4806,4808,4810,4812,4822,4824,4826,4828,4838,4840,4842,
4844,4854,4856,4858,4860,4870,4872,4874,4876,4886,4888,4890,
4892,4902,4904,4906,4908,4918,4920,4922,4924,4934,4936,4938,
4940,4950,4952,4954,4956,4966,4968,4970,4972,4982,4984,4986,
4988,4998,5000,5002,5004,5014,5016,5018,5020,5030,5032,5034,
5036,5046,5048,5050,5052,5062,5064,5066,5068,5078,5080,5082,
5084,5094,5096,5098,5100,5110,5112,5114,5116,5126,5128,5130,
5132,5142,5144,5146,5148,5158,5160,5162,5164,5174,5176,5178,
5180,5190,5192,5194,5196,5206,5208,5210,5212,5222,5224,5226,
5228,5238,5240,5242,5244,5254,5256,5258,5260,5270,5272,5274,
5276,5286,5288,5290,5292,5302,5304,5306,5308,5318,5320,5322,
5324,5334,5336,5338,5340,5350,5352,5354,5356,5366,5368,5370,
5372,5382,5384,5386,5388,5398,5400,5402,5404,5414,5416,5418,
5420,5430,5432,5434,5436,5446,5448,5450,5452,5462,5464,5466,
5468,5478,5480,5482,5484,5494,5496,5498,5500,5510,5512,5514,
5516,5526,5528,5530,5532,5542,5544,5546,5548,5558,5560,5562,
5564,5574,5576,5578,5580,5590,5592,5594,5596,5606,5608,5610,
5612,5622,5624,5626,5628,5638,5640,5642,5644,5654,5656,5658,
5660,5670,5672,5674,5676,5686,5688,5690,5692,5702,5704,5706,
5708,5718,5720,5722,5724,5734,5736,5738,5740,5750,5752,5754,
5756,5766,5768,5770,5772,5782,5784,5786,5788,5798,5800,5802,
5804,5814,5816,5818,5820,5830,5832,5834,5836,5846,5848,5850,
5852,5862,5864,5866,5868,5878,5880,5882,5884,5894,5896,5898,
5900,5910,5912,5914,5916,5926,5928,5930,5932,5942,5944,5946,
5948,5958,5960,5962,5964,5974,5976,5978,5980,5990,5992,5994,
5996,6006,6008,6010,6012,6022,6024,6026,6028,6038,6040,6042,
6044,6054,6056,6058,6060,6070,6072,6074,6076,6086,6088,6090,
6092,6102,6104,6106,6108,6118,6120,6122,6124,6134,6136,6138,
6140,6150,6152,6154,6156,6166,6168,6170,6172,6182,6184,6186,
6188,6198,6200,6202,6204,6214,6216,6218,6220,6230,6232,6234,
6236,6246,6248,6250,6252,6262,6264,6266,6268,6278,6280,6282,
6284,6294,6296,6298,6300,6310,6312,6314,6316,6326,6328,6330,
6332,6342,6344,6346,6348,6358,6360,6362,6364,6374,6376,6378,
6380,6390,6392,6394,6396,6406,6408,6410,6412,6422,6424,6426,
6428,6438,6440,6442,6444,6454,6456,6458,6460,6470,6472,6474,
6476,6486,6488,6490,6492,6502,6504,6506,6508,6518,6520,6522,
6524,6534,6536,6538,6540,6550,6552,6554,6556,6566,6568,6570,
6572,6582,6584,6586,6588,6598,6600,6602,6604,6614,6616,6618,
6620,6630,6632,6634,6636,6646,6648,6650,6652,6662,6664,6666,
6668,6678,6680,6682,6684,6694,6696,6698,6700,6710,6712,6714,
6716,6726,6728,6730,6732,6742,6744,6746,6748,6758,6760,6762,
6764,6774,6776,6778,6780,6790,6792,6794,6796,6806,6808,6810,
6812,6822,6824,6826,6828,6838,6840,6842,6844,6854,6856,6858,
6860,6870,6872,6874,6876,6886,6888,6890,6892,6902,6904,6906,
6908,6918,6920,6922,6924,6934,6936,6938,6940,6950,6952,6954,
6956,6966,6968,6970,6972,6982,6984,6986,6988,6998,7000,7002,
7004,7014,7016,7018,7020,7030,7032,7034,7036,7046,7048,7050,
7052,7062,7064,7066,7068,7078,7080,7082,7084,7094,7096,7098,
7100,7110,7112,7114,7116,7126,7128,7130,7132,7142,7144,7146,
7148,7158,7160,7162,7164,7174,7176,7178,7180,7190,7192,7194,
7196,7206,7208,7210,7212,7222,7224,7226,7228,7238,7240,7242,
7244,7254,7256,7258,7260,7270,7272,7274,7276,7286,7288,7290,
7292,7302,7304,7306,7308,7318,7320,7322,7324,7334,7336,7338,
7340,7350,7352,7354,7356,7366,7368,7370,7372,7382,7384,7386,
7388,7398,7400,7402,7404,7414,7416,7418,7420,7430,7432,7434,
7436,7446,7448,7450,7452,7462,7464,7466,7468,7478,7480,7482,
7484,7494,7496,7498,7500,7510,7512,7514,7516,7526,7528,7530,
7532,7542,7544,7546,7548,7558,7560,7562,7564,7574,7576,7578,
7580,7590,7592,7594,7596,7606,7608,7610,7612,7622,7624,7626,
7628,7638,7640,7642,7644,7654,7656,7658,7660,7670,7672,7674,
7676,7686,7688,7690,7692,7702,7704,7706,7708,7718,7720,7722,
7724,7734,7736,7738,7740,7750,7752,7754,7756,7766,7768,7770,
7772,7782,7784,7786,7788,7798,7800,7802,7804,7814,7816,7818,
7820,7830,7832,7834,7836,7846,7848,7850,7852,7862,7864,7866,
7868,7878,7880,7882,7884,7894,7896,7898,7900,7910,7912,7914,
7916,7926,7928,7930,7932,7942,7944,7946,7948,7958,7960,7962,
7964,7974,7976,7978,7980,7990,7992,7994,7996,8006,8008,8010,
8012,8022,8024,8026,8028,8038,8040,8042,8044,8054,8056,8058,
8060,8070,8072,8074,8076,8086,8088,8090,8092,8102,8104,8106,
8108,8118,8120,8122,8124,8134,8136,8138,8140,8150,8152,8154,
8156,8166,8168,8170,8172,8182,8184,8186,8188,8198,8200,8202,
8204,8214,8216,8218,8220,8230,8232,8234,8236,8246,8248,8250,
8252,8262,8264,8266,8268,8278,8280,8282,8284,8294,8296,8298,
8300,8310,8312,8314,8316,8326,8328,8330,8332,8342,8344,8346,
8348,8358,8360,8362,8364,8374,8376,8378,8380,8390,8392,8394,
8396,8406,8408,8410,8412,8422,8424,8426,8428,8438,8440,8442,
8444,8454,8456,8458,8460,8470,8472,8474,8476,8486,8488,8490,
8492,8502,8504,8506,8508,8518,8520,8522,8524,8534,8536,8538,
8540,8550,8552,8554,8556,8566,8568,8570,8572,8582,8584,8586,
8588,8598,8600,8602,8604,8614,8616,8618,8620,8630,8632,8634,
8636,8646,8648,8650,8652,8662,8664,8666,8668,8678,8680,8682,
8684,8694,8696,8698,8700,8710,8712,8714,8716,8726,8728,8730,
8732,8742,8744,8746,8748,8758,8760,8762,8764,8774,8776,8778,
8780,8790,8792,8794,8796,8806,8808,8810,8812,8822,8824,8826,
8828,8838,8840,8842,8844,8854,8856,8858,8860,8870,8872,8874,
8876,8886,8888,8890,8892,8902,8904,8906,8908,8918,8920,8922,
8924,8934,8936,8938,8940,8950,8952,8954,8956,8966,8968,8970,
8972,8982,8984,8986,8988,8998,9000,9002,9004,9014,9016,9018,
9020,9030,9032,9034,9036,9046,9048,9050,9052,9062,9064,9066,
9068,9078,9080,9082,9084,9094,9096,9098,9100,9110,9112,9114,
9116,9126,9128,9130,9132,9142,9144,9146,9148,9158,9160,9162,
9164,9174,9176,9178,9180,9190,9192,9194,9196,9206,9208,9210,
9212,9222,9224,9226,9228,9238,9240,9242,9244,9254,9256,9258,
9260,9270,9272,9274,9276,9286,9288,9290,9292,9302,9304,9306,
9308,9318,9320,9322,9324,9334,9336,9338,9340,9350,9352,9354,
9356,9366,9368,9370,9372,9382,9384,9386,9388,9398,9400,9402,
9404,9414,9416,9418,9420,9430,9432,9434,9436,9446,9448,9450,
9452,9462,9464,9466,9468,9478,9480,9482,9484,9494,9496,9498,
9500,9510,9512,9514,9516,9526,9528,9530,9532,9542,9544,9546,
9548,9558,9560,9562,9564,9574,9576,9578,9580,9590,9592,9594,
9596,9606,9608,9610,9612,9622,9624,9626,9628,9638,9640,9642,
9644,9654,9656,9658,9660,9670,9672,9674,9676,9686,9688,9690,
9692,9702,9704,9706,9708,9718,9720,9722,9724,9734,9736,9738,
9740,9750,9752,9754,9756,9766,9768,9770,9772,9782,9784,9786,
9788,9798,9800,9802,9804,9814,9816,9818,9820,9830,9832,9834,
9836,9846,9848,9850,9852,9862,9864,9866,9868,9878,9880,9882,
9884,9894,9896,9898,9900,9910,9912,9914,9916,9926,9928,9930,
9932,9942,9944,9946,9948,9958,9960,9962,9964,9974,9976,9978,
9980,9990,9992,9994,9996,10006,10008,10010,10012,10022,10024,10026,
10028,10038,10040,10042,10044,10054,10056,10058,10060,10070,10072,10074,
10076,10086,10088,10090,10092,10102,10104,10106,10108,10118,10120,10122,
10124,10134,10136,10138,10140,10150,10152,10154,10156,10166,10168,10170,
10172,10182,10184,10186,10188,10198,10200,10202,10204,10214,10216,10218,
10220,10230,10232,10234,10236,10246,10248,10250,10252,10262,10264,10266,
10268,10278,10280,10282,10284,10294,10296,10298,10300,10310,10312,10314,
10316,10326,10328,10330,10332,10342,10344,10346,10348,10358,10360,10362,
10364,10374,10376,10378,10380,10390,10392,10394,10396,10406,10408,10410,
10412,10422,10424,10426,10428,10438,10440,10442,10444,10454,10456,10458,
10460,10470,10472,10474,10476,10486,10488,10490,10492,10502,10504,10506,
10508,10518,10520,10522,10524,10534,10536,10538,10540,10550,10552,10554,
10556,10566,10568,10570,10572,10582,10584,10586,10588,10598,10600,10602,
10604,10614,10616,10618,10620,10630,10632,10634,10636,10646,10648,10650,
10652,10662,10664,10666,10668,10678,10680,10682,10684,10694,10696,10698,
10700,10710,10712,10714,10716,10726,10728,10730,10732,10742,10744,10746,
10748,10758,10760,10762,10764,10774,10776,10778,10780,10790,10792,10794,
10796,10806,10808,10810,10812,10822,10824,10826,10828,10838,10840,10842,
10844,10854,10856,10858,10860,10870,10872,10874,10876,10886,10888,10890,
10892,10902,10904,10906,10908,10918,10920,10922,10924,10934,10936,10938,
10940,10950,10952,10954,10956,10966,10968,10970,10972,10982,10984,10986,
10988,10998,11000,11002,11004,11014,11016,11018,11020,11030,11032,11034,
11036,11046,11048,11050,11052,11062,11064,11066,11068,11078,11080,11082,
11084,11094,11096,11098,11100,11110,11112,11114,11116,11126,11128,11130,
11132,11142,11144,11146,11148,11158,11160,11162,11164,11174,11176,11178,
11180,11190,11192,11194,11196,11206,11208,11210,11212,11222,11224,11226,
11228,11238,11240,11242,11244,11254,11256,11258,11260,11270,11272,11274,
11276,11286,11288,11290,11292,11302,11304,11306,11308,11318,11320,11322,
11324,11334,11336,11338,11340,11350,11352,11354,11356,11366,11368,11370,
11372,11382,11384,11386,11388,11398,11400,11402,11404,11414,11416,11418,
11420,11430,11432,11434,11436,11446,11448,11450,11452,11462,11464,11466,
11468,11478,11480,11482,11484,11494,11496,11498,11500,11510,11512,11514,
11516,11526,11528,11530,11532,11542,11544,11546,11548,11558,11560,11562,
11564,11574,11576,11578,11580,11590,11592,11594,11596,11606,11608,11610,
11612,11622,11624,11626,11628,11638,11640,11642,11644,11654,11656,11658,
11660,11670,11672,11674,11676,11686,11688,11690,11692,11702,11704,11706,
11708,11718,11720,11722,11724,11734,11736,11738,11740,11750,11752,11754,
11756,11766,11768,11770,11772,11782,11784,11786,11788,11798,11800,11802,
11804,11814,11816,11818,11820,11830,11832,11834,11836,11846,11848,11850,
11852,11862,11864,11866,11868,11878,11880,11882,11884,11894,11896,11898,
11900,11910,11912,11914,11916,11926,11928,11930,11932,11942,11944,11946,
11948,11958,11960,11962,11964,11974,11976,11978,11980,11990,11992,11994,
11996,12006,12008,12010,12012,12022,12024,12026,12028,12038,12040,12042,
12044,12054,12056,12058,12060,12070,12072,12074,12076,12086,12088,12090,
12092,12102,12104,12106,12108,12118,12120,12122,12124,12134,12136,12138,
12140,12150,12152,12154,12156,12166,12168,12170,12172,12182,12184,12186,
12188,12198,12200,12202,12204,12214,12216,12218,12220,12230,12232,12234,
12236,12246,12248,12250,12252,12262,12264,12266,12268,12278,12280,12282,
12284,12294,12296,12298,12300,12310,12312,12314,12316,12326,12328,12330,
12332,12342,12344,12346,12348,12358,12360,12362,12364,12374,12376,12378,
12380,12390,12392,12394,12396,12406,12408,12410,12412,12422,12424,12426,
12428,12438,12440,12442,12444,12454,12456,12458,12460,12470,12472,12474,
12476,12486,12488,12490,12492,12502,12504,12506,12508,12518,12520,12522,
12524,12534,12536,12538,12540,12550,12552,12554,12556,12566,12568,12570,
12572,12582,12584,12586,12588,12598,12600,12602,12604,12614,12616,12618,
12620,12630,12632,12634,12636,12646,12648,12650,12652,12662,12664,12666,
12668,12678,12680,12682,12684,12694,12696,12698,12700,12710,12712,12714,
12716,12726,12728,12730,12732,12742,12744,12746,12748,12758,12760,12762,
12764,12774,12776,12778,12780,12790,12792,12794,12796,12806,12808,12810,
12812,12822,12824,12826,12828,12838,12840,12842,12844,12854,12856,12858,
12860,12870,12872,12874,12876,12886,12888,12890,12892,12902,12904,12906,
12908,12918,12920,12922,12924,12934,12936,12938,12940,12950,12952,12954,
12956,12966,12968,12970,12972,12982,12984,12986,12988,12998,13000,13002,
13004,13014,13016,13018,13020,13030,13032,13034,13036,13046,13048,13050,
13052,13062,13064,13066,13068,13078,13080,13082,13084,13094,13096,13098,
13100,13110,13112,13114,13116,13126,13128,13130,13132,13142,13144,13146,
13148,13158,13160,13162,13164,13174,13176,13178,13180,13190,13192,13194,
13196,13206,13208,13210,13212,13222,13224,13226,13228,13238,13240,13242,
13244,13254,13256,13258,13260,13270,13272,13274,13276,13286,13288,13290,
13292,13302,13304,13306,13308,13318,13320,13322,13324,13334,13336,13338,
13340,13350,13352,13354,13356,13366,13368,13370,13372,13382,13384,13386,
13388,13398,13400,13402,13404,13414,13416,13418,13420,13430,13432,13434,
13436,13446,13448,13450,13452,13462,13464,13466,13468,13478,13480,13482,
13484,13494,13496,13498,13500,13510,13512,13514,13516,13526,13528,13530,
13532,13542,13544,13546,13548,13558,13560,13562,13564,13574,13576,13578,
13580,13590,13592,13594,13596,13606,13608,13610,13612,13622,13624,13626,
13628,13638,13640,13642,13644,13654,13656,13658,13660,13670,13672,13674,
13676,13686,13688,13690,13692,13702,13704,13706,13708,13718,13720,13722,
13724,13734,13736,13738,13740,13750,13752,13754,13756,13766,13768,13770,
13772,13782,13784,13786,13788,13798,13800,13802,13804,13814,13816,13818,
13820,13830,13832,13834,13836,13846,13848,13850,13852,13862,13864,13866,
13868,13878,13880,13882,13884,13894,13896,13898,13900,13910,13912,13914,
13916,13926,13928,13930,13932,13942,13944,13946,13948,13958,13960,13962,
13964,13974,13976,13978,13980,13990,13992,13994,13996,14006,14008,14010,
14012,14022,14024,14026,14028,14038,14040,14042,14044,14054,14056,14058,
14060,14070,14072,14074,14076,14086,14088,14090,14092,14102,14104,14106,
14108,14118,14120,14122,14124,14134,14136,14138,14140,14150,14152,14154,
14156,14166,14168,14170,14172,14182,14184,14186,14188,14198,14200,14202,
14204,14214,14216,14218,14220,14230,14232,14234,14236,14246,14248,14250,
14252,14262,14264,14266,14268,14278,14280,14282,14284,14294,14296,14298,
14300,14310,14312,14314,14316,14326,14328,14330,14332,14342,14344,14346,
14348,14358,14360,14362,14364,14374,14376,14378,14380,14390,14392,14394,
14396,14406,14408,14410,14412,14422,14424,14426,14428,14438,14440,14442,
14444,14454,14456,14458,14460,14470,14472,14474,14476,14486,14488,14490,
14492,14502,14504,14506,14508,14518,14520,14522,14524,14534,14536,14538,
14540,14550,14552,14554,14556,14566,14568,14570,14572,14582,14584,14586,
14588,14598,14600,14602,14604,14614,14616,14618,14620,14630,14632,14634,
14636,14646,14648,14650,14652,14662,14664,14666,14668,14678,14680,14682,
14684,14694,14696,14698,14700,14710,14712,14714,14716,14726,14728,14730,
14732,14742,14744,14746,14748,14758,14760,14762,14764,14774,14776,14778,
14780,14790,14792,14794,14796,14806,14808,14810,14812,14822,14824,14826,
14828,14838,14840,14842,14844,14854,14856,14858,14860,14870,14872,14874,
14876,14886,14888,14890,14892,14902,14904,14906,14908,14918,14920,14922,
14924,14934,14936,14938,14940,14950,14952,14954,14956,14966,14968,14970,
14972,14982,14984,14986,14988,14998,15000,15002,15004,15014,15016,15018,
15020,15030,15032,15034,15036,15046,15048,15050,15052,15062,15064,15066,
15068,15078,15080,15082,15084,15094,15096,15098,15100,15110,15112,15114,
15116,15126,15128,15130,15132,15142,15144,15146,15148,15158,15160,15162,
15164,15174,15176,15178,15180,15190,15192,15194,15196,15206,15208,15210,
15212,15222,15224,15226,15228,15238,15240,15242,15244,15254,15256,15258,
15260,15270,15272,15274,15276,15286,15288,15290,15292,15302,15304,15306,
15308,15318,15320,15322,15324,15334,15336,15338,15340,15350,15352,15354,
15356,15366,15368,15370,15372,15382,15384,15386,15388,15398,15400,15402,
15404,15414,15416,15418,15420,15430,15432,15434,15436,15446,15448,15450,
15452,15462,15464,15466,15468,15478,15480,15482,15484,15494,15496,15498,
15500,15510,15512,15514,15516,15526,15528,15530,15532,15542,15544,15546,
15548,15558,15560,15562,15564,15574,15576,15578,15580,15590,15592,15594,
15596,15606,15608,15610,15612,15622,15624,15626,15628,15638,15640,15642,
15644,15654,15656,15658,15660,15670,15672,15674,15676,15686,15688,15690,
15692,15702,15704,15706,15708,15718,15720,15722,15724,15734,15736,15738,
15740,15750,15752,15754,15756,15766,15768,15770,15772,15782,15784,15786,
15788,15798,15800,15802,15804,15814,15816,15818,15820,15830,15832,15834,
15836,15846,15848,15850,15852,15862,15864,15866,15868,15878,15880,15882,
15884,15894,15896,15898,15900,15910,15912,15914,15916,15926,15928,15930,
15932,15942,15944,15946,15948,15958,15960,15962,15964,15974,15976,15978,
15980,15990,15992,15994,15996,16006,16008,16010,16012,16022,16024,16026,
16028,16038,16040,16042,16044,16054,16056,16058,16060,16070,16072,16074,
16076,16086,16088,16090,16092,16102,16104,16106,16108,16118,16120,16122,
16124,16134,16136,16138,16140,16150,16152,16154,16156,16166,16168,16170,
16172,16182,16184,16186,16188,16198,16200,16202,16204,16214,16216,16218,
16220,16230,16232,16234,16236,16246,16248,16250,16252,16262,16264,16266,
16268,16278,16280,16282,16284,16294,16296,16298,16300,16310,16312,16314,
16316,16326,16328,16330,16332,16342,16344,16346,16348,16358,16360,16362,
16364,16374,16376,16378,16380,
};
static const uint32_t mdkit_pi_sd_word5_value[4097]={
0,1,2,3,4,15,18,21,24,45,50,55,
60,91,98,105,112,153,162,171,180,231,242,253,
264,325,338,351,364,435,450,465,480,561,578,595,
612,703,722,741,760,861,882,903,924,1035,1058,1081,
1104,1225,1250,1275,1300,1431,1458,1485,1512,1653,1682,1711,
1740,1891,1922,1953,1984,2145,2178,2211,2244,2415,2450,2485,
2520,2701,2738,2775,2812,3003,3042,3081,3120,3321,3362,3403,
3444,3655,3698,3741,3784,4005,4050,4095,4140,4371,4418,4465,
4512,4753,4802,4851,4900,5151,5202,5253,5304,5565,5618,5671,
5724,5995,6050,6105,6160,6441,6498,6555,6612,6903,6962,7021,
7080,7381,7442,7503,7564,7875,7938,8001,8064,8385,8450,8515,
8580,8911,8978,9045,9112,9453,9522,9591,9660,10011,10082,10153,
10224,10585,10658,10731,10804,11175,11250,11325,11400,11781,11858,11935,
12012,12403,12482,12561,12640,13041,13122,13203,13284,13695,13778,13861,
13944,14365,14450,14535,14620,15051,15138,15225,15312,15753,15842,15931,
16020,16471,16562,16653,16744,17205,17298,17391,17484,17955,18050,18145,
18240,18721,18818,18915,19012,19503,19602,19701,19800,20301,20402,20503,
20604,21115,21218,21321,21424,21945,22050,22155,22260,22791,22898,23005,
23112,23653,23762,23871,23980,24531,24642,24753,24864,25425,25538,25651,
25764,26335,26450,26565,26680,27261,27378,27495,27612,28203,28322,28441,
28560,29161,29282,29403,29524,30135,30258,30381,30504,31125,31250,31375,
31500,32131,32258,32385,32512,33153,33282,33411,33540,34191,34322,34453,
34584,35245,35378,35511,35644,36315,36450,36585,36720,37401,37538,37675,
37812,38503,38642,38781,38920,39621,39762,39903,40044,40755,40898,41041,
41184,41905,42050,42195,42340,43071,43218,43365,43512,44253,44402,44551,
44700,45451,45602,45753,45904,46665,46818,46971,47124,47895,48050,48205,
48360,49141,49298,49455,49612,50403,50562,50721,50880,51681,51842,52003,
52164,52975,53138,53301,53464,54285,54450,54615,54780,55611,55778,55945,
56112,56953,57122,57291,57460,58311,58482,58653,58824,59685,59858,60031,
60204,61075,61250,61425,61600,62481,62658,62835,63012,63903,64082,64261,
64440,65341,65522,65703,65884,66795,66978,67161,67344,68265,68450,68635,
68820,69751,69938,70125,70312,71253,71442,71631,71820,72771,72962,73153,
73344,74305,74498,74691,74884,75855,76050,76245,76440,77421,77618,77815,
78012,79003,79202,79401,79600,80601,80802,81003,81204,82215,82418,82621,
82824,83845,84050,84255,84460,85491,85698,85905,86112,87153,87362,87571,
87780,88831,89042,89253,89464,90525,90738,90951,91164,92235,92450,92665,
92880,93961,94178,94395,94612,95703,95922,96141,96360,97461,97682,97903,
98124,99235,99458,99681,99904,101025,101250,101475,101700,102831,103058,103285,
103512,104653,104882,105111,105340,106491,106722,106953,107184,108345,108578,108811,
109044,110215,110450,110685,110920,112101,112338,112575,112812,114003,114242,114481,
114720,115921,116162,116403,116644,117855,118098,118341,118584,119805,120050,120295,
120540,121771,122018,122265,122512,123753,124002,124251,124500,125751,126002,126253,
126504,127765,128018,128271,128524,129795,130050,130305,130560,131841,132098,132355,
132612,133903,134162,134421,134680,135981,136242,136503,136764,138075,138338,138601,
138864,140185,140450,140715,140980,142311,142578,142845,143112,144453,144722,144991,
145260,146611,146882,147153,147424,148785,149058,149331,149604,150975,151250,151525,
151800,153181,153458,153735,154012,155403,155682,155961,156240,157641,157922,158203,
158484,159895,160178,160461,160744,162165,162450,162735,163020,164451,164738,165025,
165312,166753,167042,167331,167620,169071,169362,169653,169944,171405,171698,171991,
172284,173755,174050,174345,174640,176121,176418,176715,177012,178503,178802,179101,
179400,180901,181202,181503,181804,183315,183618,183921,184224,185745,186050,186355,
186660,188191,188498,188805,189112,190653,190962,191271,191580,193131,193442,193753,
194064,195625,195938,196251,196564,198135,198450,198765,199080,200661,200978,201295,
201612,203203,203522,203841,204160,205761,206082,206403,206724,208335,208658,208981,
209304,210925,211250,211575,211900,213531,213858,214185,214512,216153,216482,216811,
217140,218791,219122,219453,219784,221445,221778,222111,222444,224115,224450,224785,
225120,226801,227138,227475,227812,229503,229842,230181,230520,232221,232562,232903,
233244,234955,235298,235641,235984,237705,238050,238395,238740,240471,240818,241165,
241512,243253,243602,243951,244300,246051,246402,246753,247104,248865,249218,249571,
249924,251695,252050,252405,252760,254541,254898,255255,255612,257403,257762,258121,
258480,260281,260642,261003,261364,263175,263538,263901,264264,266085,266450,266815,
267180,269011,269378,269745,270112,271953,272322,272691,273060,274911,275282,275653,
276024,277885,278258,278631,279004,280875,281250,281625,282000,283881,284258,284635,
285012,286903,287282,287661,288040,289941,290322,290703,291084,292995,293378,293761,
294144,296065,296450,296835,297220,299151,299538,299925,300312,302253,302642,303031,
303420,305371,305762,306153,306544,308505,308898,309291,309684,311655,312050,312445,
312840,314821,315218,315615,316012,318003,318402,318801,319200,321201,321602,322003,
322404,324415,324818,325221,325624,327645,328050,328455,328860,330891,331298,331705,
332112,334153,334562,334971,335380,337431,337842,338253,338664,340725,341138,341551,
341964,344035,344450,344865,345280,347361,347778,348195,348612,350703,351122,351541,
351960,354061,354482,354903,355324,357435,357858,358281,358704,360825,361250,361675,
362100,364231,364658,365085,365512,367653,368082,368511,368940,371091,371522,371953,
372384,374545,374978,375411,375844,378015,378450,378885,379320,381501,381938,382375,
382812,385003,385442,385881,386320,388521,388962,389403,389844,392055,392498,392941,
393384,395605,396050,396495,396940,399171,399618,400065,400512,402753,403202,403651,
404100,406351,406802,407253,407704,409965,410418,410871,411324,413595,414050,414505,
414960,417241,417698,418155,418612,420903,421362,421821,422280,424581,425042,425503,
425964,428275,428738,429201,429664,431985,432450,432915,433380,435711,436178,436645,
437112,439453,439922,440391,440860,443211,443682,444153,444624,446985,447458,447931,
448404,450775,451250,451725,452200,454581,455058,455535,456012,458403,458882,459361,
459840,462241,462722,463203,463684,466095,466578,467061,467544,469965,470450,470935,
471420,473851,474338,474825,475312,477753,478242,478731,479220,481671,482162,482653,
483144,485605,486098,486591,487084,489555,490050,490545,491040,493521,494018,494515,
495012,497503,498002,498501,499000,501501,502002,502503,503004,505515,506018,506521,
507024,509545,510050,510555,511060,513591,514098,514605,515112,517653,518162,518671,
519180,521731,522242,522753,523264,525825,526338,526851,527364,529935,530450,530965,
531480,534061,534578,535095,535612,538203,538722,539241,539760,542361,542882,543403,
543924,546535,547058,547581,548104,550725,551250,551775,552300,554931,555458,555985,
556512,559153,559682,560211,560740,563391,563922,564453,564984,567645,568178,568711,
569244,571915,572450,572985,573520,576201,576738,577275,577812,580503,581042,581581,
582120,584821,585362,585903,586444,589155,589698,590241,590784,593505,594050,594595,
595140,597871,598418,598965,599512,602253,602802,603351,603900,606651,607202,607753,
608304,611065,611618,612171,612724,615495,616050,616605,617160,619941,620498,621055,
621612,624403,624962,625521,626080,628881,629442,630003,630564,633375,633938,634501,
635064,637885,638450,639015,639580,642411,642978,643545,644112,646953,647522,648091,
648660,651511,652082,652653,653224,656085,656658,657231,657804,660675,661250,661825,
662400,665281,665858,666435,667012,669903,670482,671061,671640,674541,675122,675703,
676284,679195,679778,680361,680944,683865,684450,685035,685620,688551,689138,689725,
690312,693253,693842,694431,695020,697971,698562,699153,699744,702705,703298,703891,
704484,707455,708050,708645,709240,712221,712818,713415,714012,717003,717602,718201,
718800,721801,722402,723003,723604,726615,727218,727821,728424,731445,732050,732655,
733260,736291,736898,737505,738112,741153,741762,742371,742980,746031,746642,747253,
747864,750925,751538,752151,752764,755835,756450,757065,757680,760761,761378,761995,
762612,765703,766322,766941,767560,770661,771282,771903,772524,775635,776258,776881,
777504,780625,781250,781875,782500,785631,786258,786885,787512,790653,791282,791911,
792540,795691,796322,796953,797584,800745,801378,802011,802644,805815,806450,807085,
807720,810901,811538,812175,812812,816003,816642,817281,817920,821121,821762,822403,
823044,826255,826898,827541,828184,831405,832050,832695,833340,836571,837218,837865,
838512,841753,842402,843051,843700,846951,847602,848253,848904,852165,852818,853471,
854124,857395,858050,858705,859360,862641,863298,863955,864612,867903,868562,869221,
869880,873181,873842,874503,875164,878475,879138,879801,880464,883785,884450,885115,
885780,889111,889778,890445,891112,894453,895122,895791,896460,899811,900482,901153,
901824,905185,905858,906531,907204,910575,911250,911925,912600,915981,916658,917335,
918012,921403,922082,922761,923440,926841,927522,928203,928884,932295,932978,933661,
934344,937765,938450,939135,939820,943251,943938,944625,945312,948753,949442,950131,
950820,954271,954962,955653,956344,959805,960498,961191,961884,965355,966050,966745,
967440,970921,971618,972315,973012,976503,977202,977901,978600,982101,982802,983503,
984204,987715,988418,989121,989824,993345,994050,994755,995460,998991,999698,1000405,
1001112,1004653,1005362,1006071,1006780,1010331,1011042,1011753,1012464,1016025,1016738,1017451,
1018164,1021735,1022450,1023165,1023880,1027461,1028178,1028895,1029612,1033203,1033922,1034641,
1035360,1038961,1039682,1040403,1041124,1044735,1045458,1046181,1046904,1050525,1051250,1051975,
1052700,1056331,1057058,1057785,1058512,1062153,1062882,1063611,1064340,1067991,1068722,1069453,
1070184,1073845,1074578,1075311,1076044,1079715,1080450,1081185,1081920,1085601,1086338,1087075,
1087812,1091503,1092242,1092981,1093720,1097421,1098162,1098903,1099644,1103355,1104098,1104841,
1105584,1109305,1110050,1110795,1111540,1115271,1116018,1116765,1117512,1121253,1122002,1122751,
1123500,1127251,1128002,1128753,1129504,1133265,1134018,1134771,1135524,1139295,1140050,1140805,
1141560,1145341,1146098,1146855,1147612,1151403,1152162,1152921,1153680,1157481,1158242,1159003,
1159764,1163575,1164338,1165101,1165864,1169685,1170450,1171215,1171980,1175811,1176578,1177345,
1178112,1181953,1182722,1183491,1184260,1188111,1188882,1189653,1190424,1194285,1195058,1195831,
1196604,1200475,1201250,1202025,1202800,1206681,1207458,1208235,1209012,1212903,1213682,1214461,
1215240,1219141,1219922,1220703,1221484,1225395,1226178,1226961,1227744,1231665,1232450,1233235,
1234020,1237951,1238738,1239525,1240312,1244253,1245042,1245831,1246620,1250571,1251362,1252153,
1252944,1256905,1257698,1258491,1259284,1263255,1264050,1264845,1265640,1269621,1270418,1271215,
1272012,1276003,1276802,1277601,1278400,1282401,1283202,1284003,1284804,1288815,1289618,1290421,
1291224,1295245,1296050,1296855,1297660,1301691,1302498,1303305,1304112,1308153,1308962,1309771,
1310580,1314631,1315442,1316253,1317064,1321125,1321938,1322751,1323564,1327635,1328450,1329265,
1330080,1334161,1334978,1335795,1336612,1340703,1341522,1342341,1343160,1347261,1348082,1348903,
1349724,1353835,1354658,1355481,1356304,1360425,1361250,1362075,1362900,1367031,1367858,1368685,
1369512,1373653,1374482,1375311,1376140,1380291,1381122,1381953,1382784,1386945,1387778,1388611,
1389444,1393615,1394450,1395285,1396120,1400301,1401138,1401975,1402812,1407003,1407842,1408681,
1409520,1413721,1414562,1415403,1416244,1420455,1421298,1422141,1422984,1427205,1428050,1428895,
1429740,1433971,1434818,1435665,1436512,1440753,1441602,1442451,1443300,1447551,1448402,1449253,
1450104,1454365,1455218,1456071,1456924,1461195,1462050,1462905,1463760,1468041,1468898,1469755,
1470612,1474903,1475762,1476621,1477480,1481781,1482642,1483503,1484364,1488675,1489538,1490401,
1491264,1495585,1496450,1497315,1498180,1502511,1503378,1504245,1505112,1509453,1510322,1511191,
1512060,1516411,1517282,1518153,1519024,1523385,1524258,1525131,1526004,1530375,1531250,1532125,
1533000,1537381,1538258,1539135,1540012,1544403,1545282,1546161,1547040,1551441,1552322,1553203,
1554084,1558495,1559378,1560261,1561144,1565565,1566450,1567335,1568220,1572651,1573538,1574425,
1575312,1579753,1580642,1581531,1582420,1586871,1587762,1588653,1589544,1594005,1594898,1595791,
1596684,1601155,1602050,1602945,1603840,1608321,1609218,1610115,1611012,1615503,1616402,1617301,
1618200,1622701,1623602,1624503,1625404,1629915,1630818,1631721,1632624,1637145,1638050,1638955,
1639860,1644391,1645298,1646205,1647112,1651653,1652562,1653471,1654380,1658931,1659842,1660753,
1661664,1666225,1667138,1668051,1668964,1673535,1674450,1675365,1676280,1680861,1681778,1682695,
1683612,1688203,1689122,1690041,1690960,1695561,1696482,1697403,1698324,1702935,1703858,1704781,
1705704,1710325,1711250,1712175,1713100,1717731,1718658,1719585,1720512,1725153,1726082,1727011,
1727940,1732591,1733522,1734453,1735384,1740045,1740978,1741911,1742844,1747515,1748450,1749385,
1750320,1755001,1755938,1756875,1757812,1762503,1763442,1764381,1765320,1770021,1770962,1771903,
1772844,1777555,1778498,1779441,1780384,1785105,1786050,1786995,1787940,1792671,1793618,1794565,
1795512,1800253,1801202,1802151,1803100,1807851,1808802,1809753,1810704,1815465,1816418,1817371,
1818324,1823095,1824050,1825005,1825960,1830741,1831698,1832655,1833612,1838403,1839362,1840321,
1841280,1846081,1847042,1848003,1848964,1853775,1854738,1855701,1856664,1861485,1862450,1863415,
1864380,1869211,1870178,1871145,1872112,1876953,1877922,1878891,1879860,1884711,1885682,1886653,
1887624,1892485,1893458,1894431,1895404,1900275,1901250,1902225,1903200,1908081,1909058,1910035,
1911012,1915903,1916882,1917861,1918840,1923741,1924722,1925703,1926684,1931595,1932578,1933561,
1934544,1939465,1940450,1941435,1942420,1947351,1948338,1949325,1950312,1955253,1956242,1957231,
1958220,1963171,1964162,1965153,1966144,1971105,1972098,1973091,1974084,1979055,1980050,1981045,
1982040,1987021,1988018,1989015,1990012,1995003,1996002,1997001,1998000,2003001,2004002,2005003,
2006004,2011015,2012018,2013021,2014024,2019045,2020050,2021055,2022060,2027091,2028098,2029105,
2030112,2035153,2036162,2037171,2038180,2043231,2044242,2045253,2046264,2051325,2052338,2053351,
2054364,2059435,2060450,2061465,2062480,2067561,2068578,2069595,2070612,2075703,2076722,2077741,
2078760,2083861,2084882,2085903,2086924,2092035,2093058,2094081,2095104,2100225,2101250,2102275,
2103300,2108431,2109458,2110485,2111512,2116653,2117682,2118711,2119740,2124891,2125922,2126953,
2127984,2133145,2134178,2135211,2136244,2141415,2142450,2143485,2144520,2149701,2150738,2151775,
2152812,2158003,2159042,2160081,2161120,2166321,2167362,2168403,2169444,2174655,2175698,2176741,
2177784,2183005,2184050,2185095,2186140,2191371,2192418,2193465,2194512,2199753,2200802,2201851,
2202900,2208151,2209202,2210253,2211304,2216565,2217618,2218671,2219724,2224995,2226050,2227105,
2228160,2233441,2234498,2235555,2236612,2241903,2242962,2244021,2245080,2250381,2251442,2252503,
2253564,2258875,2259938,2261001,2262064,2267385,2268450,2269515,2270580,2275911,2276978,2278045,
2279112,2284453,2285522,2286591,2287660,2293011,2294082,2295153,2296224,2301585,2302658,2303731,
2304804,2310175,2311250,2312325,2313400,2318781,2319858,2320935,2322012,2327403,2328482,2329561,
2330640,2336041,2337122,2338203,2339284,2344695,2345778,2346861,2347944,2353365,2354450,2355535,
2356620,2362051,2363138,2364225,2365312,2370753,2371842,2372931,2374020,2379471,2380562,2381653,
2382744,2388205,2389298,2390391,2391484,2396955,2398050,2399145,2400240,2405721,2406818,2407915,
2409012,2414503,2415602,2416701,2417800,2423301,2424402,2425503,2426604,2432115,2433218,2434321,
2435424,2440945,2442050,2443155,2444260,2449791,2450898,2452005,2453112,2458653,2459762,2460871,
2461980,2467531,2468642,2469753,2470864,2476425,2477538,2478651,2479764,2485335,2486450,2487565,
2488680,2494261,2495378,2496495,2497612,2503203,2504322,2505441,2506560,2512161,2513282,2514403,
2515524,2521135,2522258,2523381,2524504,2530125,2531250,2532375,2533500,2539131,2540258,2541385,
2542512,2548153,2549282,2550411,2551540,2557191,2558322,2559453,2560584,2566245,2567378,2568511,
2569644,2575315,2576450,2577585,2578720,2584401,2585538,2586675,2587812,2593503,2594642,2595781,
2596920,2602621,2603762,2604903,2606044,2611755,2612898,2614041,2615184,2620905,2622050,2623195,
2624340,2630071,2631218,2632365,2633512,2639253,2640402,2641551,2642700,2648451,2649602,2650753,
2651904,2657665,2658818,2659971,2661124,2666895,2668050,2669205,2670360,2676141,2677298,2678455,
2679612,2685403,2686562,2687721,2688880,2694681,2695842,2697003,2698164,2703975,2705138,2706301,
2707464,2713285,2714450,2715615,2716780,2722611,2723778,2724945,2726112,2731953,2733122,2734291,
2735460,2741311,2742482,2743653,2744824,2750685,2751858,2753031,2754204,2760075,2761250,2762425,
2763600,2769481,2770658,2771835,2773012,2778903,2780082,2781261,2782440,2788341,2789522,2790703,
2791884,2797795,2798978,2800161,2801344,2807265,2808450,2809635,2810820,2816751,2817938,2819125,
2820312,2826253,2827442,2828631,2829820,2835771,2836962,2838153,2839344,2845305,2846498,2847691,
2848884,2854855,2856050,2857245,2858440,2864421,2865618,2866815,2868012,2874003,2875202,2876401,
2877600,2883601,2884802,2886003,2887204,2893215,2894418,2895621,2896824,2902845,2904050,2905255,
2906460,2912491,2913698,2914905,2916112,2922153,2923362,2924571,2925780,2931831,2933042,2934253,
2935464,2941525,2942738,2943951,2945164,2951235,2952450,2953665,2954880,2960961,2962178,2963395,
2964612,2970703,2971922,2973141,2974360,2980461,2981682,2982903,2984124,2990235,2991458,2992681,
2993904,3000025,3001250,3002475,3003700,3009831,3011058,3012285,3013512,3019653,3020882,3022111,
3023340,3029491,3030722,3031953,3033184,3039345,3040578,3041811,3043044,3049215,3050450,3051685,
3052920,3059101,3060338,3061575,3062812,3069003,3070242,3071481,3072720,3078921,3080162,3081403,
3082644,3088855,3090098,3091341,3092584,3098805,3100050,3101295,3102540,3108771,3110018,3111265,
3112512,3118753,3120002,3121251,3122500,3128751,3130002,3131253,3132504,3138765,3140018,3141271,
3142524,3148795,3150050,3151305,3152560,3158841,3160098,3161355,3162612,3168903,3170162,3171421,
3172680,3178981,3180242,3181503,3182764,3189075,3190338,3191601,3192864,3199185,3200450,3201715,
3202980,3209311,3210578,3211845,3213112,3219453,3220722,3221991,3223260,3229611,3230882,3232153,
3233424,3239785,3241058,3242331,3243604,3249975,3251250,3252525,3253800,3260181,3261458,3262735,
3264012,3270403,3271682,3272961,3274240,3280641,3281922,3283203,3284484,3290895,3292178,3293461,
3294744,3301165,3302450,3303735,3305020,3311451,3312738,3314025,3315312,3321753,3323042,3324331,
3325620,3332071,3333362,3334653,3335944,3342405,3343698,3344991,3346284,3352755,3354050,3355345,
3356640,3363121,3364418,3365715,3367012,3373503,3374802,3376101,3377400,3383901,3385202,3386503,
3387804,3394315,3395618,3396921,3398224,3404745,3406050,3407355,3408660,3415191,3416498,3417805,
3419112,3425653,3426962,3428271,3429580,3436131,3437442,3438753,3440064,3446625,3447938,3449251,
3450564,3457135,3458450,3459765,3461080,3467661,3468978,3470295,3471612,3478203,3479522,3480841,
3482160,3488761,3490082,3491403,3492724,3499335,3500658,3501981,3503304,3509925,3511250,3512575,
3513900,3520531,3521858,3523185,3524512,3531153,3532482,3533811,3535140,3541791,3543122,3544453,
3545784,3552445,3553778,3555111,3556444,3563115,3564450,3565785,3567120,3573801,3575138,3576475,
3577812,3584503,3585842,3587181,3588520,3595221,3596562,3597903,3599244,3605955,3607298,3608641,
3609984,3616705,3618050,3619395,3620740,3627471,3628818,3630165,3631512,3638253,3639602,3640951,
3642300,3649051,3650402,3651753,3653104,3659865,3661218,3662571,3663924,3670695,3672050,3673405,
3674760,3681541,3682898,3684255,3685612,3692403,3693762,3695121,3696480,3703281,3704642,3706003,
3707364,3714175,3715538,3716901,3718264,3725085,3726450,3727815,3729180,3736011,3737378,3738745,
3740112,3746953,3748322,3749691,3751060,3757911,3759282,3760653,3762024,3768885,3770258,3771631,
3773004,3779875,3781250,3782625,3784000,3790881,3792258,3793635,3795012,3801903,3803282,3804661,
3806040,3812941,3814322,3815703,3817084,3823995,3825378,3826761,3828144,3835065,3836450,3837835,
3839220,3846151,3847538,3848925,3850312,3857253,3858642,3860031,3861420,3868371,3869762,3871153,
3872544,3879505,3880898,3882291,3883684,3890655,3892050,3893445,3894840,3901821,3903218,3904615,
3906012,3913003,3914402,3915801,3917200,3924201,3925602,3927003,3928404,3935415,3936818,3938221,
3939624,3946645,3948050,3949455,3950860,3957891,3959298,3960705,3962112,3969153,3970562,3971971,
3973380,3980431,3981842,3983253,3984664,3991725,3993138,3994551,3995964,4003035,4004450,4005865,
4007280,4014361,4015778,4017195,4018612,4025703,4027122,4028541,4029960,4037061,4038482,4039903,
4041324,4048435,4049858,4051281,4052704,4059825,4061250,4062675,4064100,4071231,4072658,4074085,
4075512,4082653,4084082,4085511,4086940,4094091,4095522,4096953,4098384,4105545,4106978,4108411,
4109844,4117015,4118450,4119885,4121320,4128501,4129938,4131375,4132812,4140003,4141442,4142881,
4144320,4151521,4152962,4154403,4155844,4163055,4164498,4165941,4167384,4174605,4176050,4177495,
4178940,4186171,4187618,4189065,4190512,4197753,4199202,4200651,4202100,4209351,4210802,4212253,
4213704,4220965,4222418,4223871,4225324,4232595,4234050,4235505,4236960,4244241,4245698,4247155,
4248612,4255903,4257362,4258821,4260280,4267581,4269042,4270503,4271964,4279275,4280738,4282201,
4283664,4290985,4292450,4293915,4295380,4302711,4304178,4305645,4307112,4314453,4315922,4317391,
4318860,4326211,4327682,4329153,4330624,4337985,4339458,4340931,4342404,4349775,4351250,4352725,
4354200,4361581,4363058,4364535,4366012,4373403,4374882,4376361,4377840,4385241,4386722,4388203,
4389684,4397095,4398578,4400061,4401544,4408965,4410450,4411935,4413420,4420851,4422338,4423825,
4425312,4432753,4434242,4435731,4437220,4444671,4446162,4447653,4449144,4456605,4458098,4459591,
4461084,4468555,4470050,4471545,4473040,4480521,4482018,4483515,4485012,4492503,4494002,4495501,
4497000,4504501,4506002,4507503,4509004,4516515,4518018,4519521,4521024,4528545,4530050,4531555,
4533060,4540591,4542098,4543605,4545112,4552653,4554162,4555671,4557180,4564731,4566242,4567753,
4569264,4576825,4578338,4579851,4581364,4588935,4590450,4591965,4593480,4601061,4602578,4604095,
4605612,4613203,4614722,4616241,4617760,4625361,4626882,4628403,4629924,4637535,4639058,4640581,
4642104,4649725,4651250,4652775,4654300,4661931,4663458,4664985,4666512,4674153,4675682,4677211,
4678740,4686391,4687922,4689453,4690984,4698645,4700178,4701711,4703244,4710915,4712450,4713985,
4715520,4723201,4724738,4726275,4727812,4735503,4737042,4738581,4740120,4747821,4749362,4750903,
4752444,4760155,4761698,4763241,4764784,4772505,4774050,4775595,4777140,4784871,4786418,4787965,
4789512,4797253,4798802,4800351,4801900,4809651,4811202,4812753,4814304,4822065,4823618,4825171,
4826724,4834495,4836050,4837605,4839160,4846941,4848498,4850055,4851612,4859403,4860962,4862521,
4864080,4871881,4873442,4875003,4876564,4884375,4885938,4887501,4889064,4896885,4898450,4900015,
4901580,4909411,4910978,4912545,4914112,4921953,4923522,4925091,4926660,4934511,4936082,4937653,
4939224,4947085,4948658,4950231,4951804,4959675,4961250,4962825,4964400,4972281,4973858,4975435,
4977012,4984903,4986482,4988061,4989640,4997541,4999122,5000703,5002284,5010195,5011778,5013361,
5014944,5022865,5024450,5026035,5027620,5035551,5037138,5038725,5040312,5048253,5049842,5051431,
5053020,5060971,5062562,5064153,5065744,5073705,5075298,5076891,5078484,5086455,5088050,5089645,
5091240,5099221,5100818,5102415,5104012,5112003,5113602,5115201,5116800,5124801,5126402,5128003,
5129604,5137615,5139218,5140821,5142424,5150445,5152050,5153655,5155260,5163291,5164898,5166505,
5168112,5176153,5177762,5179371,5180980,5189031,5190642,5192253,5193864,5201925,5203538,5205151,
5206764,5214835,5216450,5218065,5219680,5227761,5229378,5230995,5232612,5240703,5242322,5243941,
5245560,5253661,5255282,5256903,5258524,5266635,5268258,5269881,5271504,5279625,5281250,5282875,
5284500,5292631,5294258,5295885,5297512,5305653,5307282,5308911,5310540,5318691,5320322,5321953,
5323584,5331745,5333378,5335011,5336644,5344815,5346450,5348085,5349720,5357901,5359538,5361175,
5362812,5371003,5372642,5374281,5375920,5384121,5385762,5387403,5389044,5397255,5398898,5400541,
5402184,5410405,5412050,5413695,5415340,5423571,5425218,5426865,5428512,5436753,5438402,5440051,
5441700,5449951,5451602,5453253,5454904,5463165,5464818,5466471,5468124,5476395,5478050,5479705,
5481360,5489641,5491298,5492955,5494612,5502903,5504562,5506221,5507880,5516181,5517842,5519503,
5521164,5529475,5531138,5532801,5534464,5542785,5544450,5546115,5547780,5556111,5557778,5559445,
5561112,5569453,5571122,5572791,5574460,5582811,5584482,5586153,5587824,5596185,5597858,5599531,
5601204,5609575,5611250,5612925,5614600,5622981,5624658,5626335,5628012,5636403,5638082,5639761,
5641440,5649841,5651522,5653203,5654884,5663295,5664978,5666661,5668344,5676765,5678450,5680135,
5681820,5690251,5691938,5693625,5695312,5703753,5705442,5707131,5708820,5717271,5718962,5720653,
5722344,5730805,5732498,5734191,5735884,5744355,5746050,5747745,5749440,5757921,5759618,5761315,
5763012,5771503,5773202,5774901,5776600,5785101,5786802,5788503,5790204,5798715,5800418,5802121,
5803824,5812345,5814050,5815755,5817460,5825991,5827698,5829405,5831112,5839653,5841362,5843071,
5844780,5853331,5855042,5856753,5858464,5867025,5868738,5870451,5872164,5880735,5882450,5884165,
5885880,5894461,5896178,5897895,5899612,5908203,5909922,5911641,5913360,5921961,5923682,5925403,
5927124,5935735,5937458,5939181,5940904,5949525,5951250,5952975,5954700,5963331,5965058,5966785,
5968512,5977153,5978882,5980611,5982340,5990991,5992722,5994453,5996184,6004845,6006578,6008311,
6010044,6018715,6020450,6022185,6023920,6032601,6034338,6036075,6037812,6046503,6048242,6049981,
6051720,6060421,6062162,6063903,6065644,6074355,6076098,6077841,6079584,6088305,6090050,6091795,
6093540,6102271,6104018,6105765,6107512,6116253,6118002,6119751,6121500,6130251,6132002,6133753,
6135504,6144265,6146018,6147771,6149524,6158295,6160050,6161805,6163560,6172341,6174098,6175855,
6177612,6186403,6188162,6189921,6191680,6200481,6202242,6204003,6205764,6214575,6216338,6218101,
6219864,6228685,6230450,6232215,6233980,6242811,6244578,6246345,6248112,6256953,6258722,6260491,
6262260,6271111,6272882,6274653,6276424,6285285,6287058,6288831,6290604,6299475,6301250,6303025,
6304800,6313681,6315458,6317235,6319012,6327903,6329682,6331461,6333240,6342141,6343922,6345703,
6347484,6356395,6358178,6359961,6361744,6370665,6372450,6374235,6376020,6384951,6386738,6388525,
6390312,6399253,6401042,6402831,6404620,6413571,6415362,6417153,6418944,6427905,6429698,6431491,
6433284,6442255,6444050,6445845,6447640,6456621,6458418,6460215,6462012,6471003,6472802,6474601,
6476400,6485401,6487202,6489003,6490804,6499815,6501618,6503421,6505224,6514245,6516050,6517855,
6519660,6528691,6530498,6532305,6534112,6543153,6544962,6546771,6548580,6557631,6559442,6561253,
6563064,6572125,6573938,6575751,6577564,6586635,6588450,6590265,6592080,6601161,6602978,6604795,
6606612,6615703,6617522,6619341,6621160,6630261,6632082,6633903,6635724,6644835,6646658,6648481,
6650304,6659425,6661250,6663075,6664900,6674031,6675858,6677685,6679512,6688653,6690482,6692311,
6694140,6703291,6705122,6706953,6708784,6717945,6719778,6721611,6723444,6732615,6734450,6736285,
6738120,6747301,6749138,6750975,6752812,6762003,6763842,6765681,6767520,6776721,6778562,6780403,
6782244,6791455,6793298,6795141,6796984,6806205,6808050,6809895,6811740,6820971,6822818,6824665,
6826512,6835753,6837602,6839451,6841300,6850551,6852402,6854253,6856104,6865365,6867218,6869071,
6870924,6880195,6882050,6883905,6885760,6895041,6896898,6898755,6900612,6909903,6911762,6913621,
6915480,6924781,6926642,6928503,6930364,6939675,6941538,6943401,6945264,6954585,6956450,6958315,
6960180,6969511,6971378,6973245,6975112,6984453,6986322,6988191,6990060,6999411,7001282,7003153,
7005024,7014385,7016258,7018131,7020004,7029375,7031250,7033125,7035000,7044381,7046258,7048135,
7050012,7059403,7061282,7063161,7065040,7074441,7076322,7078203,7080084,7089495,7091378,7093261,
7095144,7104565,7106450,7108335,7110220,7119651,7121538,7123425,7125312,7134753,7136642,7138531,
7140420,7149871,7151762,7153653,7155544,7165005,7166898,7168791,7170684,7180155,7182050,7183945,
7185840,7195321,7197218,7199115,7201012,7210503,7212402,7214301,7216200,7225701,7227602,7229503,
7231404,7240915,7242818,7244721,7246624,7256145,7258050,7259955,7261860,7271391,7273298,7275205,
7277112,7286653,7288562,7290471,7292380,7301931,7303842,7305753,7307664,7317225,7319138,7321051,
7322964,7332535,7334450,7336365,7338280,7347861,7349778,7351695,7353612,7363203,7365122,7367041,
7368960,7378561,7380482,7382403,7384324,7393935,7395858,7397781,7399704,7409325,7411250,7413175,
7415100,7424731,7426658,7428585,7430512,7440153,7442082,7444011,7445940,7455591,7457522,7459453,
7461384,7471045,7472978,7474911,7476844,7486515,7488450,7490385,7492320,7502001,7503938,7505875,
7507812,7517503,7519442,7521381,7523320,7533021,7534962,7536903,7538844,7548555,7550498,7552441,
7554384,7564105,7566050,7567995,7569940,7579671,7581618,7583565,7585512,7595253,7597202,7599151,
7601100,7610851,7612802,7614753,7616704,7626465,7628418,7630371,7632324,7642095,7644050,7646005,
7647960,7657741,7659698,7661655,7663612,7673403,7675362,7677321,7679280,7689081,7691042,7693003,
7694964,7704775,7706738,7708701,7710664,7720485,7722450,7724415,7726380,7736211,7738178,7740145,
7742112,7751953,7753922,7755891,7757860,7767711,7769682,7771653,7773624,7783485,7785458,7787431,
7789404,7799275,7801250,7803225,7805200,7815081,7817058,7819035,7821012,7830903,7832882,7834861,
7836840,7846741,7848722,7850703,7852684,7862595,7864578,7866561,7868544,7878465,7880450,7882435,
7884420,7894351,7896338,7898325,7900312,7910253,7912242,7914231,7916220,7926171,7928162,7930153,
7932144,7942105,7944098,7946091,7948084,7958055,7960050,7962045,7964040,7974021,7976018,7978015,
7980012,7990003,7992002,7994001,7996000,8006001,8008002,8010003,8012004,8022015,8024018,8026021,
8028024,8038045,8040050,8042055,8044060,8054091,8056098,8058105,8060112,8070153,8072162,8074171,
8076180,8086231,8088242,8090253,8092264,8102325,8104338,8106351,8108364,8118435,8120450,8122465,
8124480,8134561,8136578,8138595,8140612,8150703,8152722,8154741,8156760,8166861,8168882,8170903,
8172924,8183035,8185058,8187081,8189104,8199225,8201250,8203275,8205300,8215431,8217458,8219485,
8221512,8231653,8233682,8235711,8237740,8247891,8249922,8251953,8253984,8264145,8266178,8268211,
8270244,8280415,8282450,8284485,8286520,8296701,8298738,8300775,8302812,8313003,8315042,8317081,
8319120,8329321,8331362,8333403,8335444,8345655,8347698,8349741,8351784,8362005,8364050,8366095,
8368140,8378371,8380418,8382465,8384512,
};
static const uint32_t mdkit_pi_sd_word5_delta[4097]={
0,0,0,0,1,1,1,1,2,2,2,2,
3,3,3,3,4,4,4,4,5,5,5,5,
6,6,6,6,7,7,7,7,8,8,8,8,
9,9,9,9,10,10,10,10,11,11,11,11,
12,12,12,12,13,13,13,13,14,14,14,14,
15,15,15,15,16,16,16,16,17,17,17,17,
18,18,18,18,19,19,19,19,20,20,20,20,
21,21,21,21,22,22,22,22,23,23,23,23,
24,24,24,24,25,25,25,25,26,26,26,26,
27,27,27,27,28,28,28,28,29,29,29,29,
30,30,30,30,31,31,31,31,32,32,32,32,
33,33,33,33,34,34,34,34,35,35,35,35,
36,36,36,36,37,37,37,37,38,38,38,38,
39,39,39,39,40,40,40,40,41,41,41,41,
42,42,42,42,43,43,43,43,44,44,44,44,
45,45,45,45,46,46,46,46,47,47,47,47,
48,48,48,48,49,49,49,49,50,50,50,50,
51,51,51,51,52,52,52,52,53,53,53,53,
54,54,54,54,55,55,55,55,56,56,56,56,
57,57,57,57,58,58,58,58,59,59,59,59,
60,60,60,60,61,61,61,61,62,62,62,62,
63,63,63,63,64,64,64,64,65,65,65,65,
66,66,66,66,67,67,67,67,68,68,68,68,
69,69,69,69,70,70,70,70,71,71,71,71,
72,72,72,72,73,73,73,73,74,74,74,74,
75,75,75,75,76,76,76,76,77,77,77,77,
78,78,78,78,79,79,79,79,80,80,80,80,
81,81,81,81,82,82,82,82,83,83,83,83,
84,84,84,84,85,85,85,85,86,86,86,86,
87,87,87,87,88,88,88,88,89,89,89,89,
90,90,90,90,91,91,91,91,92,92,92,92,
93,93,93,93,94,94,94,94,95,95,95,95,
96,96,96,96,97,97,97,97,98,98,98,98,
99,99,99,99,100,100,100,100,101,101,101,101,
102,102,102,102,103,103,103,103,104,104,104,104,
105,105,105,105,106,106,106,106,107,107,107,107,
108,108,108,108,109,109,109,109,110,110,110,110,
111,111,111,111,112,112,112,112,113,113,113,113,
114,114,114,114,115,115,115,115,116,116,116,116,
117,117,117,117,118,118,118,118,119,119,119,119,
120,120,120,120,121,121,121,121,122,122,122,122,
123,123,123,123,124,124,124,124,125,125,125,125,
126,126,126,126,127,127,127,127,128,128,128,128,
129,129,129,129,130,130,130,130,131,131,131,131,
132,132,132,132,133,133,133,133,134,134,134,134,
135,135,135,135,136,136,136,136,137,137,137,137,
138,138,138,138,139,139,139,139,140,140,140,140,
141,141,141,141,142,142,142,142,143,143,143,143,
144,144,144,144,145,145,145,145,146,146,146,146,
147,147,147,147,148,148,148,148,149,149,149,149,
150,150,150,150,151,151,151,151,152,152,152,152,
153,153,153,153,154,154,154,154,155,155,155,155,
156,156,156,156,157,157,157,157,158,158,158,158,
159,159,159,159,160,160,160,160,161,161,161,161,
162,162,162,162,163,163,163,163,164,164,164,164,
165,165,165,165,166,166,166,166,167,167,167,167,
168,168,168,168,169,169,169,169,170,170,170,170,
171,171,171,171,172,172,172,172,173,173,173,173,
174,174,174,174,175,175,175,175,176,176,176,176,
177,177,177,177,178,178,178,178,179,179,179,179,
180,180,180,180,181,181,181,181,182,182,182,182,
183,183,183,183,184,184,184,184,185,185,185,185,
186,186,186,186,187,187,187,187,188,188,188,188,
189,189,189,189,190,190,190,190,191,191,191,191,
192,192,192,192,193,193,193,193,194,194,194,194,
195,195,195,195,196,196,196,196,197,197,197,197,
198,198,198,198,199,199,199,199,200,200,200,200,
201,201,201,201,202,202,202,202,203,203,203,203,
204,204,204,204,205,205,205,205,206,206,206,206,
207,207,207,207,208,208,208,208,209,209,209,209,
210,210,210,210,211,211,211,211,212,212,212,212,
213,213,213,213,214,214,214,214,215,215,215,215,
216,216,216,216,217,217,217,217,218,218,218,218,
219,219,219,219,220,220,220,220,221,221,221,221,
222,222,222,222,223,223,223,223,224,224,224,224,
225,225,225,225,226,226,226,226,227,227,227,227,
228,228,228,228,229,229,229,229,230,230,230,230,
231,231,231,231,232,232,232,232,233,233,233,233,
234,234,234,234,235,235,235,235,236,236,236,236,
237,237,237,237,238,238,238,238,239,239,239,239,
240,240,240,240,241,241,241,241,242,242,242,242,
243,243,243,243,244,244,244,244,245,245,245,245,
246,246,246,246,247,247,247,247,248,248,248,248,
249,249,249,249,250,250,250,250,251,251,251,251,
252,252,252,252,253,253,253,253,254,254,254,254,
255,255,255,255,256,256,256,256,257,257,257,257,
258,258,258,258,259,259,259,259,260,260,260,260,
261,261,261,261,262,262,262,262,263,263,263,263,
264,264,264,264,265,265,265,265,266,266,266,266,
267,267,267,267,268,268,268,268,269,269,269,269,
270,270,270,270,271,271,271,271,272,272,272,272,
273,273,273,273,274,274,274,274,275,275,275,275,
276,276,276,276,277,277,277,277,278,278,278,278,
279,279,279,279,280,280,280,280,281,281,281,281,
282,282,282,282,283,283,283,283,284,284,284,284,
285,285,285,285,286,286,286,286,287,287,287,287,
288,288,288,288,289,289,289,289,290,290,290,290,
291,291,291,291,292,292,292,292,293,293,293,293,
294,294,294,294,295,295,295,295,296,296,296,296,
297,297,297,297,298,298,298,298,299,299,299,299,
300,300,300,300,301,301,301,301,302,302,302,302,
303,303,303,303,304,304,304,304,305,305,305,305,
306,306,306,306,307,307,307,307,308,308,308,308,
309,309,309,309,310,310,310,310,311,311,311,311,
312,312,312,312,313,313,313,313,314,314,314,314,
315,315,315,315,316,316,316,316,317,317,317,317,
318,318,318,318,319,319,319,319,320,320,320,320,
321,321,321,321,322,322,322,322,323,323,323,323,
324,324,324,324,325,325,325,325,326,326,326,326,
327,327,327,327,328,328,328,328,329,329,329,329,
330,330,330,330,331,331,331,331,332,332,332,332,
333,333,333,333,334,334,334,334,335,335,335,335,
336,336,336,336,337,337,337,337,338,338,338,338,
339,339,339,339,340,340,340,340,341,341,341,341,
342,342,342,342,343,343,343,343,344,344,344,344,
345,345,345,345,346,346,346,346,347,347,347,347,
348,348,348,348,349,349,349,349,350,350,350,350,
351,351,351,351,352,352,352,352,353,353,353,353,
354,354,354,354,355,355,355,355,356,356,356,356,
357,357,357,357,358,358,358,358,359,359,359,359,
360,360,360,360,361,361,361,361,362,362,362,362,
363,363,363,363,364,364,364,364,365,365,365,365,
366,366,366,366,367,367,367,367,368,368,368,368,
369,369,369,369,370,370,370,370,371,371,371,371,
372,372,372,372,373,373,373,373,374,374,374,374,
375,375,375,375,376,376,376,376,377,377,377,377,
378,378,378,378,379,379,379,379,380,380,380,380,
381,381,381,381,382,382,382,382,383,383,383,383,
384,384,384,384,385,385,385,385,386,386,386,386,
387,387,387,387,388,388,388,388,389,389,389,389,
390,390,390,390,391,391,391,391,392,392,392,392,
393,393,393,393,394,394,394,394,395,395,395,395,
396,396,396,396,397,397,397,397,398,398,398,398,
399,399,399,399,400,400,400,400,401,401,401,401,
402,402,402,402,403,403,403,403,404,404,404,404,
405,405,405,405,406,406,406,406,407,407,407,407,
408,408,408,408,409,409,409,409,410,410,410,410,
411,411,411,411,412,412,412,412,413,413,413,413,
414,414,414,414,415,415,415,415,416,416,416,416,
417,417,417,417,418,418,418,418,419,419,419,419,
420,420,420,420,421,421,421,421,422,422,422,422,
423,423,423,423,424,424,424,424,425,425,425,425,
426,426,426,426,427,427,427,427,428,428,428,428,
429,429,429,429,430,430,430,430,431,431,431,431,
432,432,432,432,433,433,433,433,434,434,434,434,
435,435,435,435,436,436,436,436,437,437,437,437,
438,438,438,438,439,439,439,439,440,440,440,440,
441,441,441,441,442,442,442,442,443,443,443,443,
444,444,444,444,445,445,445,445,446,446,446,446,
447,447,447,447,448,448,448,448,449,449,449,449,
450,450,450,450,451,451,451,451,452,452,452,452,
453,453,453,453,454,454,454,454,455,455,455,455,
456,456,456,456,457,457,457,457,458,458,458,458,
459,459,459,459,460,460,460,460,461,461,461,461,
462,462,462,462,463,463,463,463,464,464,464,464,
465,465,465,465,466,466,466,466,467,467,467,467,
468,468,468,468,469,469,469,469,470,470,470,470,
471,471,471,471,472,472,472,472,473,473,473,473,
474,474,474,474,475,475,475,475,476,476,476,476,
477,477,477,477,478,478,478,478,479,479,479,479,
480,480,480,480,481,481,481,481,482,482,482,482,
483,483,483,483,484,484,484,484,485,485,485,485,
486,486,486,486,487,487,487,487,488,488,488,488,
489,489,489,489,490,490,490,490,491,491,491,491,
492,492,492,492,493,493,493,493,494,494,494,494,
495,495,495,495,496,496,496,496,497,497,497,497,
498,498,498,498,499,499,499,499,500,500,500,500,
501,501,501,501,502,502,502,502,503,503,503,503,
504,504,504,504,505,505,505,505,506,506,506,506,
507,507,507,507,508,508,508,508,509,509,509,509,
510,510,510,510,511,511,511,511,512,512,512,512,
513,513,513,513,514,514,514,514,515,515,515,515,
516,516,516,516,517,517,517,517,518,518,518,518,
519,519,519,519,520,520,520,520,521,521,521,521,
522,522,522,522,523,523,523,523,524,524,524,524,
525,525,525,525,526,526,526,526,527,527,527,527,
528,528,528,528,529,529,529,529,530,530,530,530,
531,531,531,531,532,532,532,532,533,533,533,533,
534,534,534,534,535,535,535,535,536,536,536,536,
537,537,537,537,538,538,538,538,539,539,539,539,
540,540,540,540,541,541,541,541,542,542,542,542,
543,543,543,543,544,544,544,544,545,545,545,545,
546,546,546,546,547,547,547,547,548,548,548,548,
549,549,549,549,550,550,550,550,551,551,551,551,
552,552,552,552,553,553,553,553,554,554,554,554,
555,555,555,555,556,556,556,556,557,557,557,557,
558,558,558,558,559,559,559,559,560,560,560,560,
561,561,561,561,562,562,562,562,563,563,563,563,
564,564,564,564,565,565,565,565,566,566,566,566,
567,567,567,567,568,568,568,568,569,569,569,569,
570,570,570,570,571,571,571,571,572,572,572,572,
573,573,573,573,574,574,574,574,575,575,575,575,
576,576,576,576,577,577,577,577,578,578,578,578,
579,579,579,579,580,580,580,580,581,581,581,581,
582,582,582,582,583,583,583,583,584,584,584,584,
585,585,585,585,586,586,586,586,587,587,587,587,
588,588,588,588,589,589,589,589,590,590,590,590,
591,591,591,591,592,592,592,592,593,593,593,593,
594,594,594,594,595,595,595,595,596,596,596,596,
597,597,597,597,598,598,598,598,599,599,599,599,
600,600,600,600,601,601,601,601,602,602,602,602,
603,603,603,603,604,604,604,604,605,605,605,605,
606,606,606,606,607,607,607,607,608,608,608,608,
609,609,609,609,610,610,610,610,611,611,611,611,
612,612,612,612,613,613,613,613,614,614,614,614,
615,615,615,615,616,616,616,616,617,617,617,617,
618,618,618,618,619,619,619,619,620,620,620,620,
621,621,621,621,622,622,622,622,623,623,623,623,
624,624,624,624,625,625,625,625,626,626,626,626,
627,627,627,627,628,628,628,628,629,629,629,629,
630,630,630,630,631,631,631,631,632,632,632,632,
633,633,633,633,634,634,634,634,635,635,635,635,
636,636,636,636,637,637,637,637,638,638,638,638,
639,639,639,639,640,640,640,640,641,641,641,641,
642,642,642,642,643,643,643,643,644,644,644,644,
645,645,645,645,646,646,646,646,647,647,647,647,
648,648,648,648,649,649,649,649,650,650,650,650,
651,651,651,651,652,652,652,652,653,653,653,653,
654,654,654,654,655,655,655,655,656,656,656,656,
657,657,657,657,658,658,658,658,659,659,659,659,
660,660,660,660,661,661,661,661,662,662,662,662,
663,663,663,663,664,664,664,664,665,665,665,665,
666,666,666,666,667,667,667,667,668,668,668,668,
669,669,669,669,670,670,670,670,671,671,671,671,
672,672,672,672,673,673,673,673,674,674,674,674,
675,675,675,675,676,676,676,676,677,677,677,677,
678,678,678,678,679,679,679,679,680,680,680,680,
681,681,681,681,682,682,682,682,683,683,683,683,
684,684,684,684,685,685,685,685,686,686,686,686,
687,687,687,687,688,688,688,688,689,689,689,689,
690,690,690,690,691,691,691,691,692,692,692,692,
693,693,693,693,694,694,694,694,695,695,695,695,
696,696,696,696,697,697,697,697,698,698,698,698,
699,699,699,699,700,700,700,700,701,701,701,701,
702,702,702,702,703,703,703,703,704,704,704,704,
705,705,705,705,706,706,706,706,707,707,707,707,
708,708,708,708,709,709,709,709,710,710,710,710,
711,711,711,711,712,712,712,712,713,713,713,713,
714,714,714,714,715,715,715,715,716,716,716,716,
717,717,717,717,718,718,718,718,719,719,719,719,
720,720,720,720,721,721,721,721,722,722,722,722,
723,723,723,723,724,724,724,724,725,725,725,725,
726,726,726,726,727,727,727,727,728,728,728,728,
729,729,729,729,730,730,730,730,731,731,731,731,
732,732,732,732,733,733,733,733,734,734,734,734,
735,735,735,735,736,736,736,736,737,737,737,737,
738,738,738,738,739,739,739,739,740,740,740,740,
741,741,741,741,742,742,742,742,743,743,743,743,
744,744,744,744,745,745,745,745,746,746,746,746,
747,747,747,747,748,748,748,748,749,749,749,749,
750,750,750,750,751,751,751,751,752,752,752,752,
753,753,753,753,754,754,754,754,755,755,755,755,
756,756,756,756,757,757,757,757,758,758,758,758,
759,759,759,759,760,760,760,760,761,761,761,761,
762,762,762,762,763,763,763,763,764,764,764,764,
765,765,765,765,766,766,766,766,767,767,767,767,
768,768,768,768,769,769,769,769,770,770,770,770,
771,771,771,771,772,772,772,772,773,773,773,773,
774,774,774,774,775,775,775,775,776,776,776,776,
777,777,777,777,778,778,778,778,779,779,779,779,
780,780,780,780,781,781,781,781,782,782,782,782,
783,783,783,783,784,784,784,784,785,785,785,785,
786,786,786,786,787,787,787,787,788,788,788,788,
789,789,789,789,790,790,790,790,791,791,791,791,
792,792,792,792,793,793,793,793,794,794,794,794,
795,795,795,795,796,796,796,796,797,797,797,797,
798,798,798,798,799,799,799,799,800,800,800,800,
801,801,801,801,802,802,802,802,803,803,803,803,
804,804,804,804,805,805,805,805,806,806,806,806,
807,807,807,807,808,808,808,808,809,809,809,809,
810,810,810,810,811,811,811,811,812,812,812,812,
813,813,813,813,814,814,814,814,815,815,815,815,
816,816,816,816,817,817,817,817,818,818,818,818,
819,819,819,819,820,820,820,820,821,821,821,821,
822,822,822,822,823,823,823,823,824,824,824,824,
825,825,825,825,826,826,826,826,827,827,827,827,
828,828,828,828,829,829,829,829,830,830,830,830,
831,831,831,831,832,832,832,832,833,833,833,833,
834,834,834,834,835,835,835,835,836,836,836,836,
837,837,837,837,838,838,838,838,839,839,839,839,
840,840,840,840,841,841,841,841,842,842,842,842,
843,843,843,843,844,844,844,844,845,845,845,845,
846,846,846,846,847,847,847,847,848,848,848,848,
849,849,849,849,850,850,850,850,851,851,851,851,
852,852,852,852,853,853,853,853,854,854,854,854,
855,855,855,855,856,856,856,856,857,857,857,857,
858,858,858,858,859,859,859,859,860,860,860,860,
861,861,861,861,862,862,862,862,863,863,863,863,
864,864,864,864,865,865,865,865,866,866,866,866,
867,867,867,867,868,868,868,868,869,869,869,869,
870,870,870,870,871,871,871,871,872,872,872,872,
873,873,873,873,874,874,874,874,875,875,875,875,
876,876,876,876,877,877,877,877,878,878,878,878,
879,879,879,879,880,880,880,880,881,881,881,881,
882,882,882,882,883,883,883,883,884,884,884,884,
885,885,885,885,886,886,886,886,887,887,887,887,
888,888,888,888,889,889,889,889,890,890,890,890,
891,891,891,891,892,892,892,892,893,893,893,893,
894,894,894,894,895,895,895,895,896,896,896,896,
897,897,897,897,898,898,898,898,899,899,899,899,
900,900,900,900,901,901,901,901,902,902,902,902,
903,903,903,903,904,904,904,904,905,905,905,905,
906,906,906,906,907,907,907,907,908,908,908,908,
909,909,909,909,910,910,910,910,911,911,911,911,
912,912,912,912,913,913,913,913,914,914,914,914,
915,915,915,915,916,916,916,916,917,917,917,917,
918,918,918,918,919,919,919,919,920,920,920,920,
921,921,921,921,922,922,922,922,923,923,923,923,
924,924,924,924,925,925,925,925,926,926,926,926,
927,927,927,927,928,928,928,928,929,929,929,929,
930,930,930,930,931,931,931,931,932,932,932,932,
933,933,933,933,934,934,934,934,935,935,935,935,
936,936,936,936,937,937,937,937,938,938,938,938,
939,939,939,939,940,940,940,940,941,941,941,941,
942,942,942,942,943,943,943,943,944,944,944,944,
945,945,945,945,946,946,946,946,947,947,947,947,
948,948,948,948,949,949,949,949,950,950,950,950,
951,951,951,951,952,952,952,952,953,953,953,953,
954,954,954,954,955,955,955,955,956,956,956,956,
957,957,957,957,958,958,958,958,959,959,959,959,
960,960,960,960,961,961,961,961,962,962,962,962,
963,963,963,963,964,964,964,964,965,965,965,965,
966,966,966,966,967,967,967,967,968,968,968,968,
969,969,969,969,970,970,970,970,971,971,971,971,
972,972,972,972,973,973,973,973,974,974,974,974,
975,975,975,975,976,976,976,976,977,977,977,977,
978,978,978,978,979,979,979,979,980,980,980,980,
981,981,981,981,982,982,982,982,983,983,983,983,
984,984,984,984,985,985,985,985,986,986,986,986,
987,987,987,987,988,988,988,988,989,989,989,989,
990,990,990,990,991,991,991,991,992,992,992,992,
993,993,993,993,994,994,994,994,995,995,995,995,
996,996,996,996,997,997,997,997,998,998,998,998,
999,999,999,999,1000,1000,1000,1000,1001,1001,1001,1001,
1002,1002,1002,1002,1003,1003,1003,1003,1004,1004,1004,1004,
1005,1005,1005,1005,1006,1006,1006,1006,1007,1007,1007,1007,
1008,1008,1008,1008,1009,1009,1009,1009,1010,1010,1010,1010,
1011,1011,1011,1011,1012,1012,1012,1012,1013,1013,1013,1013,
1014,1014,1014,1014,1015,1015,1015,1015,1016,1016,1016,1016,
1017,1017,1017,1017,1018,1018,1018,1018,1019,1019,1019,1019,
1020,1020,1020,1020,1021,1021,1021,1021,1022,1022,1022,1022,
1023,1023,1023,1023,1024,
};
static uint32_t mdkit_pi_sd_word5(unsigned raw){
 unsigned low=0,high=4097;
 while(low+1<high){unsigned mid=(low+high)/2;if(mdkit_pi_sd_word5_start[mid]<=raw)low=mid;else high=mid;}
 return (mdkit_pi_sd_word5_value[low]+mdkit_pi_sd_word5_delta[low]*(raw-mdkit_pi_sd_word5_start[low]))&0xffffff;
}
static const uint16_t mdkit_pi_sd_word6_start[128]={
0,128,256,384,512,640,768,896,1024,1152,1280,1408,
1536,1664,1792,1920,2048,2176,2304,2432,2560,2688,2816,2944,
3072,3200,3328,3456,3584,3712,3840,3968,4096,4224,4352,4480,
4608,4736,4864,4992,5120,5248,5376,5504,5632,5760,5888,6016,
6144,6272,6400,6528,6656,6784,6912,7040,7168,7296,7424,7552,
7680,7808,7936,8064,8192,8320,8448,8576,8704,8832,8960,9088,
9216,9344,9472,9600,9728,9856,9984,10112,10240,10368,10496,10624,
10752,10880,11008,11136,11264,11392,11520,11648,11776,11904,12032,12160,
12288,12416,12544,12672,12800,12928,13056,13184,13312,13440,13568,13696,
13824,13952,14080,14208,14336,14464,14592,14720,14848,14976,15104,15232,
15360,15488,15616,15744,15872,16000,16128,16256,
};
static const uint32_t mdkit_pi_sd_word6_value[128]={
5561452,5632364,5701952,5770216,5837155,5902772,5967069,6030053,6091730,6152107,6211194,6269001,
6325540,6380822,6434861,6487671,6539266,6589663,6638876,6686923,6733821,6779587,6824239,6867796,
6910275,6951695,6992075,7031434,7069791,7107166,7143577,7179043,7213585,7247220,7279968,7311847,
7342878,7373077,7402464,7431056,7458873,7485932,7512250,7537845,7562734,7586935,7610463,7633336,
7655569,7677179,7698181,7718590,7738422,7757691,7776411,7794598,7812265,7829425,7846093,7862280,
7878000,7893266,7908089,7922482,7936457,7950024,7963194,7975980,7988391,8000438,8012131,8023480,
8034495,8045184,8055557,8065624,8075392,8084870,8094067,8102990,8111647,8120047,8128195,8136101,
8143770,8151209,8158426,8165426,8172217,8178803,8185191,8191387,8197397,8203226,8208879,8214361,
8219678,8224834,8229834,8234684,8239386,8243946,8248368,8252656,8256813,8260845,8264754,8268545,
8272220,8275783,8279238,8282588,8285836,8288986,8292039,8294999,8297869,8300651,8303349,8305964,
8308499,8310957,8313340,8315650,8317890,8320061,8322165,8324205,
};
static const uint32_t mdkit_pi_sd_word6_delta[128]={
0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,
};
static uint32_t mdkit_pi_sd_word6(unsigned raw){
 unsigned low=0,high=128;
 while(low+1<high){unsigned mid=(low+high)/2;if(mdkit_pi_sd_word6_start[mid]<=raw)low=mid;else high=mid;}
 return (mdkit_pi_sd_word6_value[low]+mdkit_pi_sd_word6_delta[low]*(raw-mdkit_pi_sd_word6_start[low]))&0xffffff;
}
static const uint16_t mdkit_pi_sd_word3_start[256]={
0,64,128,192,256,320,384,448,512,576,640,704,
768,832,896,960,1024,1088,1152,1216,1280,1344,1408,1472,
1536,1600,1664,1728,1792,1856,1920,1984,2048,2112,2176,2240,
2304,2368,2432,2496,2560,2624,2688,2752,2816,2880,2944,3008,
3072,3136,3200,3264,3328,3392,3456,3520,3584,3648,3712,3776,
3840,3904,3968,4032,4096,4160,4224,4288,4352,4416,4480,4544,
4608,4672,4736,4800,4864,4928,4992,5056,5120,5184,5248,5312,
5376,5440,5504,5568,5632,5696,5760,5824,5888,5952,6016,6080,
6144,6208,6272,6336,6400,6464,6528,6592,6656,6720,6784,6848,
6912,6976,7040,7104,7168,7232,7296,7360,7424,7488,7552,7616,
7680,7744,7808,7872,7936,8000,8064,8128,8192,8256,8320,8384,
8448,8512,8576,8640,8704,8768,8832,8896,8960,9024,9088,9152,
9216,9280,9344,9408,9472,9536,9600,9664,9728,9792,9856,9920,
9984,10048,10112,10176,10240,10304,10368,10432,10496,10560,10624,10688,
10752,10816,10880,10944,11008,11072,11136,11200,11264,11328,11392,11456,
11520,11584,11648,11712,11776,11840,11904,11968,12032,12096,12160,12224,
12288,12352,12416,12480,12544,12608,12672,12736,12800,12864,12928,12992,
13056,13120,13184,13248,13312,13376,13440,13504,13568,13632,13696,13760,
13824,13888,13952,14016,14080,14144,14208,14272,14336,14400,14464,14528,
14592,14656,14720,14784,14848,14912,14976,15040,15104,15168,15232,15296,
15360,15424,15488,15552,15616,15680,15744,15808,15872,15936,16000,16064,
16128,16192,16256,16320,
};
static const uint32_t mdkit_pi_sd_word3_value[256]={
8357707,8356029,8354310,8352549,8350746,8348902,8347017,8345091,8343124,8341116,8339068,8336979,
8334850,8332681,8330472,8328224,8325935,8323607,8321240,8318833,8316388,8313903,8311380,8308818,
8306217,8303578,8300901,8298185,8295431,8292640,8289810,8286943,8284039,8281096,8278117,8275100,
8272046,8268955,8265828,8262663,8259462,8256223,8252949,8249638,8246291,8242907,8239487,8236032,
8232540,8229012,8225449,8221850,8218215,8214545,8210839,8207097,8203321,8199509,8195662,8191779,
8187862,8183909,8179922,8175900,8171842,8167750,8163624,8159462,8155266,8151035,8146770,8142470,
8138135,8133766,8129363,8124925,8120453,8115946,8111405,8106829,8102219,8097575,8092896,8088183,
8083436,8078654,8073837,8068987,8064101,8059181,8054227,8049238,8044215,8039156,8034063,8028935,
8023772,8018575,8013342,8008073,8002770,7997431,7992057,7986646,7981200,7975718,7970200,7964646,
7959055,7953427,7947762,7942059,7936320,7930542,7924727,7918873,7912980,7907048,7901077,7895066,
7889014,7882922,7876789,7870614,7864396,7858136,7851832,7845484,7839091,7832653,7826169,7819637,
7813057,7806429,7799750,7793020,7786239,7779404,7772515,7765570,7758568,7751507,7744386,7737204,
7729957,7722646,7715267,7707818,7700298,7692704,7685033,7677283,7669452,7661535,7653531,7645436,
7637245,7628957,7620566,7612068,7603459,7594735,7585890,7576919,7567817,7558577,7549193,7539658,
7529966,7520108,7510077,7499864,7489460,7478856,7468040,7457002,7445730,7434211,7422433,7410381,
7398040,7385394,7372425,7359114,7345442,7331388,7316928,7302039,7286693,7270863,7254518,7237626,
7220151,7202055,7183298,7163836,7143621,7122602,7100724,7077926,7054145,7029310,7003346,6976172,
6947698,6917831,6886467,6853495,6818796,6782240,6743689,6702993,6659992,6614515,6566376,6515380,
6461318,6403967,6343092,6278444,6209763,6136775,6059194,5976727,5889071,5795914,5696946,5591853,
5480326,5362068,5236795,5104249,4964202,4816471,4660928,4497514,4326257,4147291,3960875,3767423,
3567528,3361998,3151886,2938533,2723610,2509155,2297625,2091930,1895464,1712122,1546285,1402772,
1286732,1203475,1158225,1000000,
};
static const uint32_t mdkit_pi_sd_word3_delta[256]={
0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,
};
static uint32_t mdkit_pi_sd_word3(unsigned raw){
 unsigned low=0,high=256;
 while(low+1<high){unsigned mid=(low+high)/2;if(mdkit_pi_sd_word3_start[mid]<=raw)low=mid;else high=mid;}
 return (mdkit_pi_sd_word3_value[low]+mdkit_pi_sd_word3_delta[low]*(raw-mdkit_pi_sd_word3_start[low]))&0xffffff;
}
static void mdkit_pi_sd_packet(const float k[8],uint32_t out[8]){
 out[0]=mdkit_pi_sd_word0((unsigned)(128.0f*k[0]));
 out[1]=mdkit_pi_sd_word1((unsigned)(128.0f*k[1]));
 out[2]=mdkit_pi_sd_word2((unsigned)(128.0f*k[2]));
 out[3]=mdkit_pi_sd_word3((unsigned)(128.0f*k[7]));
 out[4]=mdkit_pi_sd_word4((unsigned)(128.0f*k[4]));
 out[5]=mdkit_pi_sd_word5((unsigned)(128.0f*k[5]));
 out[6]=mdkit_pi_sd_word6((unsigned)(128.0f*k[6]));
 out[7]=mdkit_pi_sd_word7((unsigned)(128.0f*k[3]));
}


static void sd_controls(mdkit_pi_sd*s){mdkit_pi_sd_packet(s->clock.current,s->parameters);}
static void sd_activate(mdkit_pi_sd*s){s->source_filter=s->feedback=s->feedback_previous=0;for(unsigned i=0;i<3;++i)s->delay_position[i]=s->delay_filter[i]=0;s->parameters[4]=s->parameters[5]=0x7ffcb9;s->noise_a=10;s->noise_b=1;mdkit_pi_sd_modal_clear_modes(&s->body);}
static void sd_init(void*state){mdkit_pi_sd*s=state;
  /* Reset caller-owned storage, including clock and filter history. */
  for (unsigned i = 0; i < sizeof *s; ++i) ((unsigned char *)s)[i] = 0;
mdkit_drum_zero(s);s->variant=1;mdkit_events_init(&s->clock,&s->events);mdkit_pi_sd_modal_reset(&s->body);s->pos=32;sd_controls(s);sd_activate(s);}
static void sd_target(void*state,unsigned i,unsigned v){mdkit_events_target(&((mdkit_pi_sd*)state)->events,7,i,v);}
static void sd_trigger(void*state){((mdkit_pi_sd*)state)->events.trigger=1;}
static void sd_block(mdkit_pi_sd*s){
 uint32_t*w=s->body.work,*y=s->body.fraction,*p=s->parameters;
 s->excitation_slow=mdkit_acc_to_word(mdkit_mpy(s->excitation_slow,0x7f7cee));s->excitation_fast=mdkit_acc_to_word(mdkit_mpy(s->excitation_fast,0x7c28f6));
 uint64_t tension=mdkit_add(mdkit_add(mdkit_mpy(p[2],s->excitation_fast),mdkit_mpy(p[4],s->excitation_slow)),mdkit_from24(0x400000));
 uint64_t pitch=mdkit_asl(mdkit_mpy(mdkit_acc_to_word(tension),p[0]),1);y[0xfe]=mdkit_acc_to_word(pitch);
 uint64_t step=mdkit_asr(pitch,22),phase=mdkit_from48(mdkit_pack48(s->phase_hi,s->phase_lo));
 for(unsigned i=0;i<32;++i){uint64_t stored=mdkit_to48(phase);w[i]=mdkit_mid(stored);y[i]=mdkit_low(stored);phase=mdkit_add(phase,step);}
 if(mdkit_s56(phase)>=mdkit_s56(mdkit_from24(0x1433b6)))phase=mdkit_from24(0x1433b6);s->phase_hi=mdkit_mid(phase);s->phase_lo=mdkit_low(phase);

 uint64_t first=phase,second=mdkit_add(phase,step);
 for(unsigned i=0;i<16;++i){first=(first&~UINT64_C(0xffffff))|y[2*i];second=(second&~UINT64_C(0xffffff))|y[2*i+1];first=mdkit_asr(first,1);second=mdkit_asr(second,1);uint32_t f=mdkit_low(first),g=mdkit_low(second);first=mdkit_sub(first,mdkit_from48(0x7fffff));second=mdkit_sub(second,mdkit_from48(0x7fffff));y[2*i]=f;y[2*i+1]=g;y[32+2*i]=mdkit_low(first);y[33+2*i]=mdkit_low(second);first=second=0;}
 for(unsigned i=0;i<32;++i){uint32_t index=w[i];uint64_t value=mdkit_mpy(y[32+i],drum_excitation_x(s,index));value=mdkit_sub(value,mdkit_mpy(y[i],drum_excitation_x(s,index+1)));y[64+i]=mdkit_acc_to_word(value);}
 s->rattle_tension=mdkit_acc_to_word(mdkit_mpy(0x7f5c29,s->rattle_tension));s->rattle_threshold=mdkit_acc_to_word(mdkit_mpy(p[6],s->rattle_threshold));s->rattle_gate=mdkit_acc_to_word(mdkit_mpy(0x47ae14,s->rattle_gate));
 unsigned idx=mdkit_acc_to_word(mdkit_mpy(30,p[1]));uint32_t ring=mdkit_pi_sd_modal_x(&s->body,0x142b49+idx);w[0xfb]=ring;w[0xfc]=mdkit_acc_to_word(mdkit_mpy(ring,0x5a827a));w[0xf9]=mdkit_acc_to_word(mdkit_mac(mdkit_from24(0x333333),0xd9999a,s->rattle_tension));
 drum_delays(s,p[3],w[0xf9],w[0xf9]);
 uint64_t feedback=mdkit_from24(s->feedback),input=mdkit_add(mdkit_from24(w[0x40]),feedback);uint32_t previous=s->feedback_previous;
 for(unsigned i=0;i<32;++i){uint32_t output=mdkit_acc_to_word(input);uint64_t sum=mdkit_add(mdkit_add(mdkit_from24(w[0xa0+i]),mdkit_from24(w[0xc0+i])),mdkit_from24(w[0x80+i]));w[0x40+i]=output;uint32_t old=mdkit_acc_to_word(feedback);feedback=mdkit_sub(feedback,mdkit_from24(previous));feedback=mdkit_add(feedback,sum);previous=mdkit_acc_to_word(sum);input=mdkit_add(mdkit_from24(w[0x41+i]),mdkit_from24(old));feedback=mdkit_sub(feedback,mdkit_mpy(old,0x59999a));}
 s->feedback=mdkit_acc_to_word(feedback);s->feedback_previous=previous;
 drum_modes(s,drum_coefficients(s,y[0xfe]));
}
static void sd_render(void*state,float*out,unsigned frames){mdkit_pi_sd*s=state;while(frames){if(s->pos==32){unsigned e=mdkit_events_boundary(&s->clock,&s->events,0);if(e&MDKIT_PI_PUBLISH)sd_controls(s);if(e&MDKIT_PI_FIRST)sd_activate(s);if(e&MDKIT_PI_TRIGGER){s->phase_hi=0x142f4b;s->phase_lo=s->rattle_gate=0;s->excitation_fast=s->excitation_slow=s->rattle_tension=s->rattle_threshold=0x7fffff;}sd_block(s);s->pos=0;}unsigned n=32-s->pos;if(n>frames)n=frames;for(unsigned i=0;i<n;++i)out[i] = s->events.triggered ? (float)mdkit_s24(s->block[s->pos + i]) / 8388608.0f : 0.0f;frames-=n;out+=n;s->pos+=n;}}
static const char*const names[]={"PTCH","DEC","HARD","RING","TENS","RVOL","RDEC"};
const mdkit_voice mdkit_voice_pi_sd={"PI-SD",65,7,names,sizeof(mdkit_pi_sd),sd_init,sd_target,sd_trigger,sd_render, mdkit_pi_sd_defaults
};
