/* SPDX-License-Identifier: CC0-1.0
 * PI-CC standalone synthesis voice.
 *
 * Oscillator sources use eight-sample polynomial segments. Damping and
 * envelope coefficients use an inverse-time exponential model. Metallic waveshape and
 * coefficient banks use documented closed-form constructions.
 */
#include "mdkit_pi_cc.h"
#include <stdint.h>

/* ---- Default controls --------------------------------------------------- */
/* Active controls are in descriptor order; unused clock slots stay at 64. */
static const unsigned char mdkit_pi_cc_defaults[MDKIT_MAX_CONTROLS] = {
    64, 70, 90, 32, 127, 127, 64, 127
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
static __attribute__((unused)) double mdkit_filter_curve(const double c[15], double row)
{
  double x=(row-63.5)/63.5,b1=0.0,b2=0.0;
  for(unsigned k=14;k>0;--k){double b=2.0*x*b1-b2+c[k];b2=b1;b1=b;}
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

static uint32_t cc_damp(unsigned index)
{ return mdkit_decay_radius(index, 1u); }


static uint32_t cc_env(unsigned index)
{ return mdkit_decay_radius(index, 128u); }

static const uint32_t cc_boot_pts[40] = {
  -64684,-60117,-55696,-51429,-47330,-43399,-39645,-36069,-32671,-29456,-26424,-23582,
  -20923,-18448,-16152,-14023,-12084,-10315,-8720,-7305,-6017,-4903,-3939,-3118,
  -2538,-2055,-1726,-1554,-178961,-178463,-177580,-176183,-174297,-171981,-169241,-166119,
  -162653,-158867,-154801,-150485,
};

static const double cc_A_coef[4][15] = {
    {-4916160.6753321178, 2394598.511154416, 220825.25023971143, -56021.636600646387, -19858.95305666421, -2043.4769451174225, 239.89229204265774, 109.76644140061843, 13.676402598696049, -0.72359435699830854, -0.55754831760769508, -0.1013214734557544, -0.036193063468600732, 0.081688821675593382, 0.011459581132924392},
    {1703035.3363232373, -2177600.8628359484, -462931.24857188168, -47189.936424824897, 2363.0941450805599, 1749.6735489314458, 281.66923654401893, 4.7026887508156499, -7.6355070583837907, -1.717191470453785, -0.12800600232974135, 0.077238194211821445, 0.022528321085800742, 0.072900517942144194, 0.011212444373830859},
    {3143400.1183507731, -835975.05933771876, -140549.07391283254, -4200.3176471150973, 3132.936984135104, 687.08196197879079, 37.229711612002205, -12.987040862452092, -3.6505939369148912, -0.24596866558636463, -0.023679625945375687, 7.201349985132044e-05, -0.0077348270932106212, -0.0012005648538482552, -0.0028455999699675953},
    {-6262923.579821202, 1702949.9831517311, 292245.69822967506, 10778.676837820956, -6026.6638413380479, -1397.1165764283999, -87.230525282383084, 24.235129231125317, 7.2609377453461281, 0.74099236662905466, -0.069824883292837078, -0.071783832739049119, 0.017749614326091387, -0.019865895850534075, -0.043904972757157108},
};

static const double cc_B_coef[4][15] = {
    {-2485571.7176408991, 137212.93781955843, -583453.07397491962, -213249.6271213117, -61701.822101968166, -16276.042223549641, -3966.2334097090188, -894.59720567475176, -185.12282016265735, -34.198701543061851, -5.1012084542043699, -0.45611258237047342, 0.091620048990202457, 0.094143231898590848, 0.016528336567190376},
    {-3603907.6011297214, -3921304.5363406632, -710634.36284218763, -125115.51131602969, -19927.412835108742, -2152.9631925015001, 140.26281700521662, 185.82032171614935, 77.180656558559747, 24.822314809424856, 6.9996357167132341, 1.8240758416286611, 0.38453556550342877, 0.10223865941171204, -0.014651859760951607},
    {1479637.2156396001, 785795.08791199839, 231966.88313865903, 57113.061655765116, 13180.231109071667, 2838.8536597784191, 555.14837721813001, 93.482862318483953, 11.406294752387987, -0.097348898807958287, -0.67205438816476759, -0.33794227092238338, -0.097407494368834779, -0.097083067003444753, -0.014415242350789382},
    {1887419.0226333812, 2281107.8460207111, 538427.22876195551, 117514.19986346322, 24417.922933521044, 4613.3228389207934, 732.63580995292114, 74.436853666697942, -7.4212469077496666, -7.6332556742163202, -3.0546804544911992, -1.0235545578717655, -0.33641961938197712, -0.14065883317648445, -0.057287938461823604},
};

static const uint32_t cc_hh_row[4] = {
  0xdbb5c5u,0xf10ab7u,0x210ef4u,0xbeee14u,
};

static uint32_t cc_boot_x(unsigned a)
{ if(a==0u)return 0xfffcdbu; if(a==1u)return 0x000323u; return 0u; }
static uint32_t cc_boot_y(unsigned a)
{
  static const uint32_t head[4]={0x405218u,0xbf1c44u,0u,0u};
  if(a<4u)return head[a]; if(a==320u)return 0x100u; if(a==321u)return 0x800u;
  if(a>=355u&&a<512u){unsigned q=(a-355u)/4u,f=(a-355u)%4u;uint32_t x=cc_boot_pts[q];
    if(a==511u)return x; uint32_t y=cc_boot_pts[q+1u]; return (uint32_t)((int64_t)x+((int64_t)y-(int64_t)x)*(int64_t)f/4)&0xffffffu;}
  return 0u;
}

static uint32_t cc_rom(uint32_t address)
{
  uint32_t a=address&0xffffffu;
  if (a>=0x142949u && a<0x142b49u) return cc_damp(a-0x142949u);
  if (a>=0x142b49u && a<0x142d49u) return cc_env(a-0x142b49u);
  if (a >= 0x148000u && a < 0x150000u) { unsigned i=(a-0x148000u)&0x7fffu; return mdkit_word(8388605.0*mdkit_sin_turn(i,32767.0)); }
  if (a >= 0x145224u && a < 0x145424u) { unsigned r=(a-0x145224u)/4u,c=(a-0x145224u)%4u; return mdkit_word(mdkit_filter_curve(cc_A_coef[c],(double)r)); }
  if (a >= 0x145424u && a < 0x145624u) { unsigned r=(a-0x145424u)/4u,c=(a-0x145424u)%4u; return mdkit_word(mdkit_filter_curve(cc_B_coef[c],(double)r)); }
  if (a >= 0x145420u && a < 0x145424u) return cc_hh_row[a-0x145420u];
  return 0u;
}


enum { MDKIT_PI_PUBLISH = 1, MDKIT_PI_TRIGGER = 2, MDKIT_PI_FIRST = 4 };
static inline void mdkit_events_init(mdkit_control_clock *c, mdkit_pi_cc_events *e) {

  for(unsigned i=0;i<8u;++i){const float v=(float)mdkit_pi_cc_defaults[i];c->target[i]=c->raw[i]=c->current[i]=c->pending[i]=v;}
  c->count = 8; c->block = 0; c->publish_block = 0; c->pending_valid = 0;
  c->next_update = 2;
  e->targets = e->trigger = e->triggered = 0;
  for (unsigned i = 0; i < 8; ++i) {
    e->queued[i] = mdkit_pi_cc_defaults[i];
    e->last_raw[i] = (unsigned)mdkit_pi_cc_defaults[i] * 128u;
  }
}
static inline void mdkit_events_target(mdkit_pi_cc_events *e, unsigned count,
                                       unsigned i, unsigned value) {
  if (i < count) {
    e->queued[i] = value > 127 ? 127 : value;
    e->targets |= 1u << i;
  }
}
static inline unsigned
mdkit_events_boundary(mdkit_control_clock *c, mdkit_pi_cc_events *e, int trigger_only) {
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

static inline __attribute__((unused)) uint32_t mdkit_pi_cc_modal_x(mdkit_pi_cc_modal *c, uint32_t address) {
  if (address < 512u) return c->work[address];
  if (address >= 0x142000u && address < 0x145000u) return cc_rom(address);
  return cc_boot_x(address);
}
static inline __attribute__((unused)) uint32_t mdkit_pi_cc_modal_y(mdkit_pi_cc_modal *c, uint32_t address) {
  if (address < 512u) return c->fraction[address];
  return (address >= 0x142000u && address < 0x145000u) ? cc_rom(address) : 0u;
}
static inline __attribute__((unused)) void mdkit_pi_cc_modal_reset(mdkit_pi_cc_modal *c) {
  for (unsigned i = 0; i < 512; ++i) {
    c->work[i] = cc_boot_x(i);
    c->fraction[i] = cc_boot_y(i);
  }
  for (unsigned i = 0; i < 8; ++i)
    for (unsigned j = 0; j < 3; ++j)
      c->history[i][j] = 0;
}
static inline __attribute__((unused)) void mdkit_pi_cc_modal_clear_modes(mdkit_pi_cc_modal *c) {
  for (unsigned i = 0; i < 8; ++i)
    for (unsigned j = 0; j < 3; ++j)
      c->history[i][j] = 0;
}
static inline __attribute__((unused)) uint32_t mdkit_pi_cc_modal_wrap(uint32_t p, int step, unsigned m) {
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
static inline __attribute__((unused)) void mdkit_pi_cc_modal_excitation(mdkit_pi_cc_modal *c, uint32_t *hi,
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
  uint32_t first = mdkit_pi_cc_modal_x(c, c->work[0]);
  for (unsigned i = 0; i < 32; ++i) {
    uint32_t index = c->work[i];
    uint64_t a = mdkit_mpy_su(first, c->fraction[32 + i]);
    a = mdkit_add(a, mdkit_mpy_su(mdkit_pi_cc_modal_x(c, index + 1), c->fraction[i]));
    first = mdkit_pi_cc_modal_x(c, c->work[i + 1]);
    c->work[64 + i] = mdkit_acc_to_word(a);
  }
}
/* The eight modes receive a cubic normalization polynomial and measured
 * damping-table taps. Their five MACs preserve the two parallel transfers. */
static inline __attribute__((unused)) void mdkit_pi_cc_modal_coefficients(mdkit_pi_cc_modal *c, uint32_t pitch,
                                         uint32_t decay, uint32_t freq_table,
                                         uint32_t amp_table) {
  uint32_t *y = c->fraction;
  for (unsigned i = 0; i < 8; ++i)
    y[0xb4 + i] = mdkit_acc_to_word(mdkit_mpy(mdkit_pi_cc_modal_y(c, freq_table + i), pitch));
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
        mdkit_acc_to_word(mdkit_mpy(mdkit_pi_cc_modal_x(c, amp_table - 8 + i), decay));
    uint32_t damping = mdkit_pi_cc_modal_x(c, 0x142949 + idx);
    uint64_t a =
        mdkit_add(mdkit_asl(mdkit_mpy(damping, y[0xa0 + i]), 1), mdkit_from24(0x800000));
    uint32_t first = mdkit_acc_to_word(a);
    uint64_t b = mdkit_sub(0, mdkit_mpy(damping, damping));
    a = mdkit_add(a, b);
    uint32_t normalized = mdkit_acc_to_word(a);
    uint64_t gain = mdkit_asl(mdkit_mpy(mdkit_pi_cc_modal_x(c, amp_table + i), normalized), 1);
    y[0x64 + 4 * i] = mdkit_acc_to_word(b);
    y[0x65 + 4 * i] = first;
    y[0x66 + 4 * i] = mdkit_acc_to_word(gain);
  }
}
static inline __attribute__((unused)) void mdkit_pi_cc_modal_filter(mdkit_pi_cc_modal *c) {
  uint32_t *w = c->work, *y = c->fraction;

  for (unsigned mode = 0; mode < 8; ++mode) {
    unsigned coef = 0x64 + mode * 4, read = 0x60, write = 0x40;
    uint64_t a = mdkit_from24(c->history[mode][0]);
    a = (a & ~UINT64_C(0xffffff)) | c->history[mode][1];
    uint32_t x1 = c->history[mode][2], x0 = w[read++];
    uint32_t y0 = y[coef];
    coef = mdkit_pi_cc_modal_wrap(coef, 1, 2);
    uint32_t old = mdkit_acc_to_word(a);
    a = mdkit_mac(a, x1, y0);
    x1 = old;
    y0 = y[coef];
    coef = mdkit_pi_cc_modal_wrap(coef, 1, 2);
    a = mdkit_mac(a, x1, y0);
    y0 = y[coef];
    coef = mdkit_pi_cc_modal_wrap(coef, 1, 2);
    uint64_t b = mdkit_from24(w[write]);
    a = mdkit_mac(a, y0, x0);
    y0 = y[coef];
    coef = mdkit_pi_cc_modal_wrap(coef, 1, 2);
    for (unsigned i = 0; i < 31; ++i) {
      b = mdkit_add(b, a);
      x0 = w[read++];
      old = mdkit_acc_to_word(a);
      a = mdkit_mac(a, x1, y0);
      x1 = old;
      y0 = y[coef];
      coef = mdkit_pi_cc_modal_wrap(coef, 1, 2);
      a = mdkit_mac(a, x1, y0);
      w[write++] = mdkit_acc_to_word(b);
      y0 = y[coef];
      coef = mdkit_pi_cc_modal_wrap(coef, 1, 2);
      b = mdkit_from24(w[write]);
      a = mdkit_mac(a, y0, x0);
      y0 = y[coef];
      coef = mdkit_pi_cc_modal_wrap(coef, 1, 2);
    }
    b = mdkit_add(b, a);
    w[write] = mdkit_acc_to_word(b);

    c->history[mode][0] = mdkit_acc_to_word(a);
    c->history[mode][1] = mdkit_low(a);
    c->history[mode][2] = x1;
  }
}

static inline uint32_t metal_advance(uint32_t old,int delta,uint32_t modulo)
{
 if(modulo==0xffffff)return(old+(uint32_t)delta)&MD_MASK24;
 if(!modulo)return old;
 uint32_t mask=1;while(mask<=modulo)mask<<=1;--mask;
 uint32_t base=old&~mask;int64_t value=(int64_t)old+delta;
 int64_t span=((uint32_t)delta&mask)?(int64_t)modulo+1:0;
 if(value<base)value+=span;if(value>(int64_t)base+modulo)value-=span;
 return(uint32_t)value&MD_MASK24;
}
static inline uint32_t metal_read(mdkit_pi_cc*s,unsigned fraction,unsigned address)
{
 address&=MD_MASK24;
 if(address>=0x800&&address<0x840&&fraction)return s->storage.state_words[address-0x800];
 if(address<1024)return fraction?s->fraction[address]:s->work[address];
 if(address>=0x130000&&address<0x140000)return s->delay[address-0x130000];
 if(address>=0x140000&&address<0x150000)return cc_rom(address);
 return 0;
}
static inline void metal_write(mdkit_pi_cc*s,unsigned fraction,unsigned address,uint32_t word)
{
 address&=MD_MASK24;word&=MD_MASK24;
 if(address>=0x800&&address<0x840&&fraction)s->storage.state_words[address-0x800]=word;
 else if(address<1024){if(fraction)s->fraction[address]=word;else s->work[address]=word;}
 else if(address>=0x130000&&address<0x140000)s->delay[address-0x130000]=word;
}
static inline void metal_reset(mdkit_pi_cc*s)
{
 mdkit_events_init(&s->clock,&s->events);s->pos=32;s->machine=0;
 for(unsigned i=0;i<64;++i)s->storage.state_words[i]=0;
 for(unsigned i=0;i<1024;++i){s->work[i]=i<512u?cc_boot_x(i):0u;s->fraction[i]=i<512u?cc_boot_y(i):0u;}
 for(unsigned i=0;i<65536u;++i)s->delay[i]=0;
 for(unsigned i=0;i<32;++i)s->block[i]=0;
}

static inline uint32_t metal_product(uint32_t a,uint32_t b){return mdkit_acc_to_word(mdkit_mpy(a,b));}
static inline void metal_activate(mdkit_pi_cc*s)
{
 for(unsigned i=0x1c;i<0x30;++i)s->storage.state_words[i]=0;
 uint32_t base=0x135600+mdkit_low(mdkit_mpy(0x300,s->fraction[0x142]));
 s->storage.voice.page_a=base;s->storage.voice.page_d=base+0x100;s->storage.voice.page_c=base+0x200;s->storage.voice.page_b=base+0x300;
 s->storage.voice.decay=0x7fdf3b;s->storage.voice.noise_a=10;s->storage.voice.noise_b=1;
 s->storage.voice.impulse_step=0x51eb8;s->storage.voice.excitation_decay=0x79999a;s->storage.voice.excitation_threshold=0xb33333;
 if(s->machine==70)s->storage.voice.attack_step=0x266666;
 if(s->machine==72){s->storage.voice.close_memory=0;s->storage.voice.attack_step=0xccccd;}
 if(s->machine==71)s->storage.voice.close_memory=s->storage.voice.amplitude=0;
}
static inline void metal_noise(mdkit_pi_cc*s,unsigned pairs)
{
 uint64_t first=0,second=mdkit_from24(s->storage.voice.noise_a);uint32_t word=s->storage.voice.noise_b;
 for(unsigned i=0;i<pairs;++i){second=mdkit_add(second,mdkit_from24(word));first=(mdkit_from24(word)&~(uint64_t)MD_MASK24)|mdkit_low(first);
  s->fraction[2*i]=word;word=mdkit_mid(second);first=mdkit_add(first,mdkit_from24(word));s->fraction[2*i+1]=word;word=mdkit_mid(first);}
 s->storage.voice.noise_a=word;s->storage.voice.noise_b=mdkit_mid(second);
}
/* Shared finite excitation window. RC adds X sine into fractional workspace32;
 * CC/HH add Y sine into modal workspace0. The last look-ahead is not stored. */
static inline void metal_window(mdkit_pi_cc*s,uint32_t step,uint32_t gain,int ride)
{
 uint64_t budget=mdkit_from24(s->storage.voice.window_budget),limit=mdkit_from24(step);
 if(mdkit_s56(budget)>=mdkit_s56(limit))return;
 budget=mdkit_add(budget,mdkit_from24(32));s->storage.voice.window_budget=mdkit_acc_to_word(budget);
 uint64_t remaining=mdkit_sub(limit,budget);
 if(mdkit_s56(remaining)>((int64_t)32<<24))remaining=mdkit_from24(32);
 if(mdkit_s56(remaining)<0)remaining=mdkit_sub(0,remaining);
 unsigned pairs=mdkit_mid(mdkit_asr(remaining,1))&0xffff;if(!pairs)pairs=65536;
 unsigned base=ride?32:0;uint32_t cursor=s->storage.voice.excitation_cursor,stride=s->storage.voice.window_step;
 uint32_t sample=metal_read(s,!ride,cursor);cursor=metal_advance(cursor,mdkit_s24(stride),0x7fff);
 uint64_t next=mdkit_mac(mdkit_from24(metal_read(s,ride,base)),sample,gain);
 unsigned input=base+1,output=base;
 for(unsigned i=0;i<pairs;++i){
  sample=metal_read(s,!ride,cursor);cursor=metal_advance(cursor,mdkit_s24(stride),0x7fff);
  uint64_t first=mdkit_mac(mdkit_from24(metal_read(s,ride,input++)),sample,gain);
  metal_write(s,ride,output++,mdkit_acc_to_word(next));
  sample=metal_read(s,!ride,cursor);cursor=metal_advance(cursor,mdkit_s24(stride),0x7fff);
  next=mdkit_mac(mdkit_from24(metal_read(s,ride,input++)),sample,gain);
  metal_write(s,ride,output++,mdkit_acc_to_word(first));
 }
 s->storage.voice.excitation_cursor=metal_advance(cursor,-(int)mdkit_s24(stride),0x7fff);
}
static inline void metal_excitation(mdkit_pi_cc*s,uint32_t increment,uint32_t threshold)
{
 uint64_t envelope=mdkit_from24(s->storage.voice.excitation_envelope);
 for(unsigned i=0;i<32;++i){if(mdkit_s24(s->fraction[i])>mdkit_s24(threshold))envelope=mdkit_add(envelope,mdkit_from24(increment));
  uint32_t word=mdkit_acc_to_word(envelope);s->work[8+i]=word;envelope=mdkit_mpy(word,s->storage.voice.excitation_decay);}
 s->storage.voice.excitation_envelope=mdkit_acc_to_word(envelope);
 uint32_t noise=s->fraction[32];
 for(unsigned i=0;i<32;++i){s->work[8+i]=metal_product(noise,s->work[8+i]);noise=s->fraction[i];}
 if(s->machine==72&&mdkit_s24(noise)>=0x79999a)s->storage.voice.excitation_envelope=mdkit_acc_to_word(mdkit_add(mdkit_from24(s->storage.voice.excitation_envelope),mdkit_from24(0x333333)));
}
/* Five-coefficient second-order recurrence. The duplicated leading MAC and
 * long-word output overlap are retained rather than replaced by a textbook
 * biquad. Three voices share this exact section for both body stages and output. */
static inline void metal_filter(mdkit_pi_cc*s,unsigned base,uint32_t*history,uint32_t*fraction,uint32_t*previous,const uint32_t coeff[5])
{
 for(unsigned i=0;i<5;++i)s->fraction[40+i]=coeff[i];
 unsigned input=base,output=base+2;
 uint64_t running=mdkit_from48(mdkit_pack48(*history,*fraction)),stored=0;
 uint32_t sample=s->work[input++],cached=s->work[input++];
 running=mdkit_mac(running,coeff[0],sample);running=mdkit_mac(running,coeff[1],cached);
 for(unsigned i=0;i<32;++i){
  running=mdkit_mac(running,coeff[1],cached);
  sample=s->work[input++];running=mdkit_mac(running,coeff[2],sample);
  sample=s->work[input++];running=mdkit_mac(running,coeff[3],sample);
  sample=s->work[input];input-=3;running=mdkit_mac(running,coeff[4],sample);
  sample=s->work[input++];stored=running;
  (void)s->work[input++];cached=mdkit_acc_to_word(running);
  running=mdkit_mac(running,coeff[0],sample);running=mdkit_mac(running,coeff[1],cached);
  uint64_t limited=mdkit_to48(stored);s->work[output]=mdkit_mid(limited);s->fraction[output]=mdkit_low(limited);++output;
 }
 *history=s->work[output-1];*fraction=mdkit_low(stored);*previous=s->work[output-2];
}
static inline void metal_body_filters(mdkit_pi_cc*s)
{
 uint32_t coeff[5];unsigned row=4*s->storage.voice.brightness;
 s->work[4]=s->storage.voice.first_previous;s->work[5]=s->storage.voice.first_history;s->work[6]=s->storage.voice.first_input_previous;s->work[7]=s->storage.voice.first_input;
 for(unsigned i=0;i<4;++i)coeff[i]=metal_read(s,0,0x145224+row+i);coeff[4]=coeff[2];
 s->storage.voice.first_input=s->work[39];s->storage.voice.first_input_previous=s->work[38];
 metal_filter(s,4,&s->storage.voice.first_history,&s->storage.voice.first_fraction,&s->storage.voice.first_previous,coeff);
 s->work[0]=s->storage.voice.second_previous;s->work[1]=s->storage.voice.second_history;s->work[2]=s->storage.voice.second_input_previous;s->work[3]=s->storage.voice.second_input;
 unsigned second_row=s->machine==70?4*s->storage.voice.coupling_angle:mdkit_mid(mdkit_asl(mdkit_mpy(s->storage.voice.close_memory,s->storage.voice.coupling_angle),2))&0xfffffc;
 for(unsigned i=0;i<4;++i)coeff[i]=metal_read(s,0,0x145424+second_row+i);coeff[4]=coeff[2];
 s->storage.voice.second_input=s->work[35];s->storage.voice.second_input_previous=s->work[34];
 metal_filter(s,0,&s->storage.voice.second_history,&s->storage.voice.second_fraction,&s->storage.voice.second_previous,coeff);
}
static inline void metal_amplitude(mdkit_pi_cc*s)
{
 uint64_t running=mdkit_from24(s->storage.voice.amplitude);uint32_t gain=mdkit_acc_to_word(running);
 uint64_t product=mdkit_mpy(gain,s->work[0]);uint32_t next=s->work[1];
 if(s->storage.voice.attack_gate){
  uint32_t step=s->storage.voice.attack_gate;running=mdkit_add(running,mdkit_from24(step));
  for(unsigned i=0;i<31;++i){s->work[i]=mdkit_acc_to_word(product);product=mdkit_mpy(gain,next);next=s->work[i+2];gain=mdkit_acc_to_word(running);running=mdkit_add(running,mdkit_from24(step));}
  s->work[31]=mdkit_acc_to_word(product);s->storage.voice.amplitude=mdkit_acc_to_word(running);
  if(mdkit_s56(running)>=(INT64_C(0x7fffff)<<24))s->storage.voice.attack_gate=0;
 }else{
  uint32_t decay=s->storage.voice.decay;if(mdkit_s24(s->storage.voice.ring_amplitude)<84)decay=s->machine==72?0x7f8d50:0x666666;
  running=mdkit_mpy(gain,decay);
  for(unsigned i=0;i<31;++i){s->work[i]=mdkit_acc_to_word(product);product=mdkit_mpy(gain,next);next=s->work[i+2];uint32_t old=mdkit_acc_to_word(running);running=mdkit_mpy(gain,decay);gain=old;}
  s->work[31]=mdkit_acc_to_word(product);s->storage.voice.amplitude=mdkit_mid(running);
 }
}
/* Each feedback read stores the OLD accumulator before adding its new source.
 * The next negative product uses that old transfer word, with full signed mpy. */
static inline void metal_delay_read(mdkit_pi_cc*s,uint32_t page_a,uint32_t page_b,uint32_t stride_a,uint32_t stride_b,uint32_t*history_a,uint32_t*history_b,unsigned out_a,unsigned out_b)
{
 uint32_t a=metal_advance(page_a,mdkit_s24(stride_a),0xff),b=metal_advance(page_b,mdkit_s24(stride_b),0xff);
 uint32_t source_a=metal_read(s,0,a),source_b=metal_read(s,0,b);a=metal_advance(a,1,0xff);b=metal_advance(b,1,0xff);
 uint64_t first=mdkit_from24(*history_a),second=mdkit_from24(*history_b);uint32_t gain=s->fraction[7];
 for(unsigned i=0;i<8;++i){uint32_t old_a=mdkit_acc_to_word(first),old_b=mdkit_acc_to_word(second);s->work[out_a+i]=old_a;s->work[out_b+i]=old_b;
  first=mdkit_mac(first,gain,source_a);second=mdkit_mac(second,gain,source_b);
  first=mdkit_sub(first,mdkit_mpy(gain,old_a));second=mdkit_sub(second,mdkit_mpy(gain,old_b));
  source_a=metal_read(s,0,a);source_b=metal_read(s,0,b);a=metal_advance(a,1,0xff);b=metal_advance(b,1,0xff);
 }
 *history_a=mdkit_acc_to_word(first);*history_b=mdkit_acc_to_word(second);
}
static inline void metal_delay_write(mdkit_pi_cc*s,unsigned base,unsigned other,unsigned source,uint32_t*page_a,uint32_t*page_b,int negative)
{
 uint32_t gain=s->fraction[0],a=*page_a,b=*page_b,drive=s->machine==70?0x59999a:0x666666;
 uint64_t first=mdkit_mpy(gain,s->work[base]),second=first;if(negative)first=second=mdkit_sub(0,first);
 second=mdkit_sub(second,mdkit_mpy(gain,s->work[other]));first=mdkit_mac(first,gain,s->work[other]);
 second=mdkit_mac(second,drive,s->work[source]);first=mdkit_mac(first,drive,s->work[source]);
 for(unsigned i=0;i<8;++i){metal_write(s,1,b,mdkit_acc_to_word(second));b=metal_advance(b,1,0xff);metal_write(s,1,a,mdkit_acc_to_word(first));a=metal_advance(a,1,0xff);
  first=mdkit_mpy(gain,s->work[base+i+1]);second=first;if(negative)first=second=mdkit_sub(0,first);
  second=mdkit_sub(second,mdkit_mpy(gain,s->work[other+i+1]));first=mdkit_mac(first,gain,s->work[other+i+1]);
  second=mdkit_mac(second,drive,s->work[source+i+1]);first=mdkit_mac(first,drive,s->work[source+i+1]);
 }
 *page_a=a;*page_b=b;
}
static inline void metal_coupling(mdkit_pi_cc*s)
{
 unsigned source=0,audio=0x100;s->fraction[6]=0;
 uint32_t delay_gain=s->machine==70?0x733333:0x7eb852,excitation_gain=s->machine==72?0x400000:0xccccd;
 for(unsigned pass=0;pass<4;++pass){
  metal_delay_read(s,s->storage.voice.page_a,s->storage.voice.page_d,s->fraction[2],s->fraction[5],&s->storage.voice.delay_history_a,&s->storage.voice.delay_history_d,0x89,0xc9);
  metal_delay_read(s,s->storage.voice.page_c,s->storage.voice.page_b,s->fraction[4],s->fraction[3],&s->storage.voice.delay_history_c,&s->storage.voice.delay_history_b,0xc0,0x80);
  audio=metal_advance(audio,-1,31);uint32_t prior=s->fraction[audio];
  for(unsigned i=0;i<8;++i){uint64_t mixed=mdkit_mac(mdkit_mpy(delay_gain,s->work[0xc9+i]),delay_gain,s->work[0xc0+i]);mixed=mdkit_mac(mixed,excitation_gain,s->work[source+i]);
   s->fraction[audio]=prior;audio=metal_advance(audio,1,31);prior=mdkit_acc_to_word(mixed);}
  s->fraction[audio]=prior;audio=metal_advance(audio,1,31);
  metal_delay_write(s,0x80,0xc0,source,&s->storage.voice.page_a,&s->storage.voice.page_d,1);
  metal_delay_write(s,0x89,0xc9,source,&s->storage.voice.page_b,&s->storage.voice.page_c,0);
  source+=8;s->fraction[6]=source;
 }
}
static inline void metal_output(mdkit_pi_cc*s,uint32_t gain)
{
 unsigned duration=s->machine==70?16:s->machine==71?6:8;
 uint32_t pulse=s->machine==71?0x333333:0x666666;
 if(s->machine==72?s->storage.voice.gate!=0x7fffff:!s->storage.voice.gate)pulse=0;
 for(unsigned i=0;i<32;++i)s->work[4+i]=i<duration?pulse:0;
 uint32_t coeff[5];unsigned row=4*s->storage.voice.brightness;
 s->work[0]=s->storage.voice.output_previous;s->work[1]=s->storage.voice.output_history;s->work[2]=s->storage.voice.output_input_previous;s->work[3]=s->storage.voice.output_input;
 unsigned table=s->machine==72?0x145420:0x145224+row;
 coeff[0]=metal_read(s,0,table);coeff[1]=metal_read(s,0,table+1);
 coeff[2]=metal_product(gain,metal_read(s,0,table+2));coeff[3]=metal_product(gain,metal_read(s,0,table+3));coeff[4]=coeff[2];
 s->storage.voice.output_input=s->work[35];s->storage.voice.output_input_previous=s->work[34];
 metal_filter(s,0,&s->storage.voice.output_history,&s->storage.voice.output_fraction,&s->storage.voice.output_previous,coeff);
 unsigned input=0x100,output=0x1ff;uint64_t pending=mdkit_from24(s->fraction[output]);
 uint64_t current=mdkit_from24(s->fraction[input]);input=metal_advance(input,1,31);unsigned work=4;
 uint32_t sample=s->work[work++];
 for(unsigned i=0;i<16;++i){current=mdkit_add(current,mdkit_from24(sample));s->fraction[output]=mdkit_acc_to_word(pending);output=metal_advance(output,1,0xff);
  current=mdkit_asl(current,1);sample=s->work[work++];pending=mdkit_from24(s->fraction[input]);input=metal_advance(input,1,31);
  pending=mdkit_add(pending,mdkit_from24(sample));sample=s->work[work++];pending=mdkit_asl(pending,1);
  s->fraction[output]=mdkit_acc_to_word(current);output=metal_advance(output,1,0xff);current=mdkit_from24(s->fraction[input]);input=metal_advance(input,1,31);
 }
 s->fraction[output]=mdkit_acc_to_word(pending);s->storage.voice.gate=0;
}
static inline void metal_coefficients(mdkit_pi_cc*s)
{
 uint32_t lookup=s->storage.voice.ring,scale=500;
 if(s->machine==70){lookup=mdkit_acc_to_word(mdkit_add(mdkit_mpy(s->storage.voice.hardness,0x7fdf3b),mdkit_from24(lookup)));scale=511;}
 if(s->machine!=72&&mdkit_s24(s->storage.voice.ring_amplitude)<84)lookup=0xccccd;
 unsigned index=mdkit_acc_to_word(mdkit_mpy(scale,lookup));s->fraction[0]=metal_product(metal_read(s,0,0x142b49+index),0x5a827a);
 s->fraction[7]=mdkit_acc_to_word(mdkit_mac(mdkit_from24(0x733333),0xccccd,s->storage.voice.coupling_gain));
 static const uint32_t ride_steps[4]={0xffff83,0xffff95,0xffffa9,0xffffbf};
 static const uint32_t cymbal_steps[4]={0xffff07,0xffff7b,0xffff8b,0xffffb7};
 uint32_t pitch=s->storage.voice.pitch;if(s->machine==72&&s->storage.voice.gate==2)pitch=metal_product(pitch,0x400000);
 for(unsigned i=0;i<4;++i)s->fraction[2+i]=metal_product(pitch,s->machine==70?ride_steps[i]:cymbal_steps[i]);
}


static const uint16_t mdkit_pi_cc_word0_start[256]={
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
static const uint32_t mdkit_pi_cc_word0_value[256]={
8388607,8343791,8299439,8251615,8208239,8165327,8119023,8077039,8031727,7986911,7946271,7902415,
7859039,7816127,7773695,7731711,7690175,7649087,7605071,7564879,7525119,7482511,7443615,7401919,
7363839,7323039,7282687,7242767,7203295,7164239,7125615,7087391,7049583,7012175,6972335,6935743,
6896767,6860959,6822815,6785087,6750431,6713503,6676975,6640831,6605087,6567215,6532255,6497679,
6461023,6427183,6391311,6358191,6323087,6288383,6254031,6220079,6186479,6153247,6120351,6087823,
6053503,6021679,5988095,5954879,5924079,5891583,5859423,5827631,5796175,5765055,5734255,5701903,
5671791,5640127,5610655,5579679,5549039,5518735,5488767,5459119,5429791,5400767,5372063,5341983,
5313903,5284479,5255375,5226607,5199711,5171535,5142127,5114575,5087311,5058847,5032175,5004319,
4978223,4950959,4923999,4897327,4870943,4844847,4817679,4792143,4765551,4740575,4714559,4688815,
4663359,4638191,4613279,4587407,4563039,4537727,4513887,4489119,4464607,4440383,4416415,4392703,
4369231,4344927,4321967,4298175,4274655,4251375,4228367,4205583,4194303,4171887,4149711,4125807,
4104111,4082655,4059503,4038511,4015855,3993455,3973135,3951199,3929519,3908063,3886847,3865855,
3845087,3824543,3802527,3782431,3762559,3741247,3721807,3700959,3681919,3661519,3641343,3621375,
3601647,3582111,3562799,3543695,3524783,3506079,3486159,3467871,3448383,3430479,3411407,3392543,
3375215,3356751,3338479,3320415,3302543,3283599,3266127,3248831,3230511,3213583,3195647,3179087,
3161535,3144191,3127007,3110031,3093231,3076623,3060175,3043903,3026751,3010831,2994047,2977439,
2962031,2945791,2929711,2913807,2898079,2882527,2867119,2850943,2835887,2820063,2805327,2789839,
2774511,2759359,2744383,2729551,2714895,2700383,2686031,2670991,2656943,2642239,2627679,2613295,
2599855,2585759,2571055,2557279,2543647,2529423,2516079,2502159,2489103,2475471,2461999,2448655,
2435471,2422415,2408831,2396063,2382767,2370287,2357279,2344399,2331679,2319087,2306639,2293695,
2281519,2268863,2256943,2244559,2232303,2220191,2208207,2196351,2184607,2172463,2160975,2149087,
2137327,2125679,2114175,2102783,
};
static const uint32_t mdkit_pi_cc_word0_delta[256]={
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
static uint32_t mdkit_pi_cc_word0(unsigned raw){
 unsigned low=0,high=256;
 while(low+1<high){unsigned mid=(low+high)/2;if(mdkit_pi_cc_word0_start[mid]<=raw)low=mid;else high=mid;}
 return (mdkit_pi_cc_word0_value[low]+mdkit_pi_cc_word0_delta[low]*(raw-mdkit_pi_cc_word0_start[low]))&0xffffff;
}
static const uint16_t mdkit_pi_cc_word4_start[256]={
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
static const uint32_t mdkit_pi_cc_word4_value[256]={
7189,7224,7260,7303,7347,7383,7421,7466,7504,7550,7581,7629,
7668,7709,7757,7798,7840,7882,7924,7967,8011,8055,8099,8144,
8189,8235,8281,8319,8366,8414,8463,8502,8551,8601,8642,8693,
8744,8786,8839,8882,8925,8979,9023,9079,9124,9181,9227,9274,
9332,9380,9428,9477,9538,9588,9638,9689,9740,9792,9845,9898,
9965,10005,10074,10130,10172,10228,10285,10343,10402,10461,10521,10566,
10627,10689,10752,10799,10863,10927,10976,11042,11109,11159,11228,11279,
11349,11402,11473,11527,11599,11655,11729,11785,11842,11919,11977,12056,
12115,12176,12236,12318,12381,12443,12507,12593,12658,12723,12790,12857,
12925,12993,13062,13156,13203,13299,13347,13444,13519,13568,13644,13720,
13798,13876,13955,14035,14115,14197,14252,14336,14392,14448,14534,14621,
14708,14767,14857,14947,15008,15101,15163,15258,15353,15418,15515,15614,
15681,15781,15849,15953,16022,16128,16199,16306,16379,16489,16563,16638,
16752,16829,16945,17024,17103,17223,17304,17386,17510,17594,17678,17764,
17850,17981,18069,18159,18249,18385,18478,18571,18665,18761,18857,18954,
19102,19202,19302,19404,19507,19611,19716,19823,19930,20039,20149,20260,
20372,20485,20600,20716,20833,20952,21072,21133,21255,21379,21504,21630,
21758,21888,21953,22085,22218,22353,22490,22559,22698,22839,22982,23054,
23199,23347,23496,23571,23723,23877,23955,24112,24271,24352,24514,24679,
24762,24930,25014,25186,25359,25447,25625,25714,25895,25987,26172,26359,
26454,26646,26743,26939,27038,27137,27339,27441,27648,27752,27963,28070,
28286,28395,28505,28728,
};
static const uint32_t mdkit_pi_cc_word4_delta[256]={
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
static uint32_t mdkit_pi_cc_word4(unsigned raw){
 unsigned low=0,high=256;
 while(low+1<high){unsigned mid=(low+high)/2;if(mdkit_pi_cc_word4_start[mid]<=raw)low=mid;else high=mid;}
 return (mdkit_pi_cc_word4_value[low]+mdkit_pi_cc_word4_delta[low]*(raw-mdkit_pi_cc_word4_start[low]))&0xffffff;
}
static const uint16_t mdkit_pi_cc_word1_start[231]={
0,72,143,214,285,356,427,498,569,640,712,783,
854,925,996,1067,1138,1209,1280,1352,1423,1494,1565,1636,
1707,1778,1849,1920,1992,2063,2134,2205,2276,2347,2418,2489,
2560,2632,2703,2774,2845,2916,2987,3058,3129,3200,3272,3343,
3414,3485,3556,3627,3698,3769,3840,3912,3983,4054,4125,4196,
4267,4338,4409,4480,4552,4623,4694,4765,4836,4907,4978,5049,
5120,5192,5263,5334,5405,5476,5547,5618,5689,5760,5832,5903,
5974,6045,6116,6187,6258,6329,6400,6472,6543,6614,6685,6756,
6827,6898,6969,7040,7112,7183,7254,7325,7396,7467,7538,7609,
7680,7752,7823,7894,7965,8036,8107,8178,8249,8320,8392,8463,
8534,8605,8676,8747,8818,8889,8960,9032,9103,9174,9245,9316,
9387,9458,9529,9600,9672,9743,9814,9885,9956,10027,10098,10169,
10240,10312,10383,10454,10525,10596,10667,10738,10809,10880,10952,11023,
11094,11165,11236,11307,11378,11449,11520,11592,11663,11734,11805,11876,
11947,12018,12089,12160,12232,12303,12374,12445,12516,12587,12658,12729,
12800,12872,12943,13014,13085,13156,13227,13298,13369,13440,13512,13583,
13654,13725,13796,13867,13938,14009,14080,14152,14223,14294,14365,14436,
14507,14578,14649,14720,14792,14863,14934,15005,15076,15147,15218,15289,
15360,15432,15503,15574,15645,15716,15787,15858,15929,16000,16072,16143,
16214,16285,16356,
};
static const uint32_t mdkit_pi_cc_word1_value[231]={
8241397,8244784,8248094,8251328,8254489,8257577,8260595,8263544,8266426,8269241,8271992,8274681,
8277307,8279874,8282381,8284831,8287225,8289565,8291850,8294083,8296265,8298397,8300479,8302514,
8304502,8306445,8308343,8310197,8312008,8313778,8315508,8317197,8318847,8320460,8322035,8323575,
8325078,8326547,8327982,8329385,8330754,8332093,8333400,8334677,8335925,8337144,8338335,8339498,
8340635,8341745,8342830,8343890,8344925,8345936,8346924,8347889,8348832,8349753,8350653,8351532,
8352390,8353229,8354049,8354849,8355631,8356395,8357141,8357870,8358582,8359278,8359958,8360622,
8361270,8361904,8362522,8363127,8363717,8364294,8364858,8365408,8365946,8366471,8366984,8367486,
8367975,8368454,8368921,8369377,8369823,8370259,8370684,8371100,8371506,8371902,8372290,8372668,
8373038,8373399,8373751,8374096,8374432,8374761,8375082,8375396,8375702,8376002,8376294,8376580,
8376859,8377131,8377397,8377657,8377911,8378159,8378402,8378639,8378870,8379096,8379316,8379532,
8379742,8379948,8380149,8380345,8380537,8380724,8380907,8381086,8381260,8381431,8381597,8381760,
8381919,8382074,8382226,8382374,8382518,8382660,8382798,8382932,8383064,8383193,8383318,8383441,
8383561,8383678,8383792,8383904,8384013,8384120,8384224,8384326,8384425,8384522,8384617,8384710,
8384800,8384888,8384975,8385059,8385141,8385222,8385300,8385377,8385452,8385525,8385597,8385667,
8385735,8385802,8385867,8385930,8385993,8386053,8386113,8386170,8386227,8386282,8386336,8386389,
8386441,8386491,8386540,8386588,8386635,8386681,8386725,8386769,8386812,8386853,8386894,8386934,
8386973,8387011,8387048,8387084,8387119,8387154,8387188,8387221,8387253,8387284,8387315,8387345,
8387374,8387403,8387431,8387458,8387485,8387511,8387536,8387561,8387586,8387609,8387632,8387655,
8387677,8387699,8387720,8387741,8387761,8387780,8387800,8387818,8387837,8387855,8387872,8387889,
8387906,8387922,8387938,
};
static const uint32_t mdkit_pi_cc_word1_delta[231]={
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
0,0,0,
};
static uint32_t mdkit_pi_cc_word1(unsigned raw){
 unsigned low=0,high=231;
 while(low+1<high){unsigned mid=(low+high)/2;if(mdkit_pi_cc_word1_start[mid]<=raw)low=mid;else high=mid;}
 return (mdkit_pi_cc_word1_value[low]+mdkit_pi_cc_word1_delta[low]*(raw-mdkit_pi_cc_word1_start[low]))&0xffffff;
}
static const uint16_t mdkit_pi_cc_word2_start[127]={
0,128,256,384,512,640,896,1024,1152,1280,1408,1536,
1664,1792,1920,2048,2176,2304,2432,2560,2688,2816,2944,3072,
3200,3328,3456,3584,3712,3840,3968,4096,4224,4352,4480,4608,
4736,4864,4992,5120,5248,5376,5504,5632,5760,5888,6016,6144,
6272,6400,6528,6656,6784,6912,7040,7168,7296,7424,7552,7680,
7808,7936,8064,8192,8320,8448,8576,8704,8832,8960,9088,9216,
9344,9472,9600,9728,9856,9984,10112,10240,10368,10496,10624,10752,
10880,11008,11136,11264,11392,11520,11648,11776,11904,12032,12160,12288,
12416,12544,12672,12800,12928,13056,13184,13312,13440,13568,13696,13824,
13952,14080,14208,14336,14464,14592,14720,14848,14976,15104,15232,15360,
15488,15616,15744,15872,16000,16128,16256,
};
static const uint32_t mdkit_pi_cc_word2_value[127]={
0,640,1152,1280,1408,1536,1664,1792,1920,2048,2304,2432,
2560,2816,2944,3200,3456,3712,3968,4224,4608,4864,5248,5632,
6144,6528,7040,7552,8064,8704,9344,9984,10752,11520,12416,13312,
14208,15232,16384,17664,18944,20352,21760,23424,25088,27008,28928,31104,
33408,35840,38400,41216,44288,47488,51072,54784,58752,63104,67712,72704,
78080,83840,89984,96512,103680,111232,119424,128128,137600,147712,158592,170240,
182656,196096,210560,225920,242560,260352,279552,300032,322048,345728,371072,398336,
427648,459008,492672,528896,567808,609408,654208,702208,753792,809216,868608,932352,
1000832,1074432,1153280,1238016,1328896,1426432,1531264,1643648,1764352,1894016,2033024,2182400,
2342656,2514688,2699264,2897536,3110400,3338752,3584000,3847168,4129664,4432896,4758528,5107968,
5483008,5885696,6317952,6781952,7280000,7814656,8388480,
};
static const uint32_t mdkit_pi_cc_word2_delta[127]={
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
0,0,0,0,0,0,0,
};
static uint32_t mdkit_pi_cc_word2(unsigned raw){
 unsigned low=0,high=127;
 while(low+1<high){unsigned mid=(low+high)/2;if(mdkit_pi_cc_word2_start[mid]<=raw)low=mid;else high=mid;}
 return (mdkit_pi_cc_word2_value[low]+mdkit_pi_cc_word2_delta[low]*(raw-mdkit_pi_cc_word2_start[low]))&0xffffff;
}
static const uint16_t mdkit_pi_cc_word3_start[1]={
0,
};
static const uint32_t mdkit_pi_cc_word3_value[1]={
262144,
};
static const uint32_t mdkit_pi_cc_word3_delta[1]={
64,
};
static uint32_t mdkit_pi_cc_word3(unsigned raw){
 unsigned low=0,high=1;
 while(low+1<high){unsigned mid=(low+high)/2;if(mdkit_pi_cc_word3_start[mid]<=raw)low=mid;else high=mid;}
 return (mdkit_pi_cc_word3_value[low]+mdkit_pi_cc_word3_delta[low]*(raw-mdkit_pi_cc_word3_start[low]))&0xffffff;
}
static const uint16_t mdkit_pi_cc_word5_start[1]={
0,
};
static const uint32_t mdkit_pi_cc_word5_value[1]={
0,
};
static const uint32_t mdkit_pi_cc_word5_delta[1]={
512,
};
static uint32_t mdkit_pi_cc_word5(unsigned raw){
 unsigned low=0,high=1;
 while(low+1<high){unsigned mid=(low+high)/2;if(mdkit_pi_cc_word5_start[mid]<=raw)low=mid;else high=mid;}
 return (mdkit_pi_cc_word5_value[low]+mdkit_pi_cc_word5_delta[low]*(raw-mdkit_pi_cc_word5_start[low]))&0xffffff;
}
static const uint16_t mdkit_pi_cc_word6_start[128]={
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
static const uint32_t mdkit_pi_cc_word6_value[128]={
0,1,2,3,4,5,6,7,8,9,10,11,
12,13,14,15,16,17,18,19,20,21,22,23,
24,25,26,27,28,29,30,31,32,33,34,35,
36,37,38,39,40,41,42,43,44,45,46,47,
48,49,50,51,52,53,54,55,56,57,58,59,
60,61,62,63,64,65,66,67,68,69,70,71,
72,73,74,75,76,77,78,79,80,81,82,83,
84,85,86,87,88,89,90,91,92,93,94,95,
96,97,98,99,100,101,102,103,104,105,106,107,
108,109,110,111,112,113,114,115,116,117,118,119,
120,121,122,123,124,125,126,127,
};
static const uint32_t mdkit_pi_cc_word6_delta[128]={
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
static uint32_t mdkit_pi_cc_word6(unsigned raw){
 unsigned low=0,high=128;
 while(low+1<high){unsigned mid=(low+high)/2;if(mdkit_pi_cc_word6_start[mid]<=raw)low=mid;else high=mid;}
 return (mdkit_pi_cc_word6_value[low]+mdkit_pi_cc_word6_delta[low]*(raw-mdkit_pi_cc_word6_start[low]))&0xffffff;
}
static const uint16_t mdkit_pi_cc_word7_start[128]={
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
static const uint32_t mdkit_pi_cc_word7_value[128]={
0,1,2,3,4,5,6,7,8,9,10,11,
12,13,14,15,16,17,18,19,20,21,22,23,
24,25,26,27,28,29,30,31,32,33,34,35,
36,37,38,39,40,41,42,43,44,45,46,47,
48,49,50,51,52,53,54,55,56,57,58,59,
60,61,62,63,64,65,66,67,68,69,70,71,
72,73,74,75,76,77,78,79,80,81,82,83,
84,85,86,87,88,89,90,91,92,93,94,95,
96,97,98,99,100,101,102,103,104,105,106,107,
108,109,110,111,112,113,114,115,116,117,118,119,
120,121,122,123,124,125,126,127,
};
static const uint32_t mdkit_pi_cc_word7_delta[128]={
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
static uint32_t mdkit_pi_cc_word7(unsigned raw){
 unsigned low=0,high=128;
 while(low+1<high){unsigned mid=(low+high)/2;if(mdkit_pi_cc_word7_start[mid]<=raw)low=mid;else high=mid;}
 return (mdkit_pi_cc_word7_value[low]+mdkit_pi_cc_word7_delta[low]*(raw-mdkit_pi_cc_word7_start[low]))&0xffffff;
}
static const uint16_t mdkit_pi_cc_word8_start[229]={
0,72,143,214,285,356,427,498,569,640,712,783,
854,925,996,1067,1138,1209,1280,1352,1423,1494,1565,1636,
1707,1778,1849,1920,1992,2063,2134,2205,2276,2347,2418,2489,
2560,2632,2703,2774,2845,2916,2987,3058,3129,3200,3272,3343,
3414,3485,3556,3627,3698,3769,3840,3912,3983,4054,4125,4196,
4267,4338,4409,4480,4552,4623,4694,4765,4836,4907,4978,5049,
5120,5192,5263,5334,5405,5476,5547,5618,5689,5760,5832,5903,
5974,6045,6116,6187,6258,6329,6400,6472,6543,6614,6685,6756,
6827,6898,6969,7040,7112,7183,7254,7325,7396,7467,7538,7609,
7680,7752,7823,7894,7965,8036,8107,8178,8249,8320,8392,8463,
8534,8605,8676,8747,8818,8889,8960,9032,9103,9174,9245,9316,
9387,9458,9529,9600,9672,9743,9814,9885,9956,10027,10098,10169,
10240,10312,10383,10454,10525,10596,10667,10738,10809,10880,10952,11023,
11094,11165,11236,11307,11378,11449,11520,11592,11663,11734,11805,11876,
11947,12018,12089,12160,12232,12303,12374,12445,12516,12587,12658,12729,
12800,12872,12943,13014,13085,13156,13227,13298,13369,13440,13512,13583,
13654,13725,13796,13867,13938,14009,14080,14152,14223,14294,14365,14436,
14507,14578,14649,14720,14792,14863,14934,15005,15076,15147,15218,15289,
15360,15432,15503,15574,15645,15716,15787,15858,15929,16000,16072,16143,
16192,
};
static const uint32_t mdkit_pi_cc_word8_value[229]={
1322358,1399852,1479288,1560574,1643614,1728309,1814555,1902247,1991278,2081539,2172920,2265310,
2358600,2452678,2547435,2642763,2738556,2834707,2931115,3027679,3124300,3220885,3317341,3413578,
3509513,3605062,3700147,3794694,3888631,3981891,4074409,4166127,4256986,4346936,4435926,4523911,
4610848,4696700,4781430,4865006,4947400,5028585,5108539,5187241,5264674,5340823,5415675,5489221,
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
8308499,8310957,8313340,8315650,8317890,8320061,8322165,8324205,8326183,8328101,8329959,8331761,
8333508,8335201,8336842,8338433,8339975,8341470,8342919,8344324,8345685,8347005,8348284,8349524,
8350727,8351892,8353021,8354116,8355177,8356205,8357202,8358169,8359105,8360013,8360893,8361746,
8362573,8363374,8364151,8364904,8365633,8366341,8367026,8367690,8368334,8368959,8369564,8370150,
8370718,8371269,8371803,8372321,8372822,8373308,8373780,8374236,8374679,8375108,8375524,8375927,
8388607,
};
static const uint32_t mdkit_pi_cc_word8_delta[229]={
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
0,
};
static uint32_t mdkit_pi_cc_word8(unsigned raw){
 unsigned low=0,high=229;
 while(low+1<high){unsigned mid=(low+high)/2;if(mdkit_pi_cc_word8_start[mid]<=raw)low=mid;else high=mid;}
 return (mdkit_pi_cc_word8_value[low]+mdkit_pi_cc_word8_delta[low]*(raw-mdkit_pi_cc_word8_start[low]))&0xffffff;
}
static void mdkit_pi_cc_packet(const float k[8],uint32_t out[9]){
 out[0]=mdkit_pi_cc_word0((unsigned)(128.0f*k[0]));
 out[1]=mdkit_pi_cc_word1((unsigned)(128.0f*k[1]));
 out[2]=mdkit_pi_cc_word2((unsigned)(128.0f*k[2]));
 out[3]=mdkit_pi_cc_word3((unsigned)(128.0f*k[3]));
 out[4]=mdkit_pi_cc_word4((unsigned)(128.0f*k[0]));
 out[5]=mdkit_pi_cc_word5((unsigned)(128.0f*k[4]));
 out[6]=mdkit_pi_cc_word6((unsigned)(128.0f*k[5]));
 out[7]=mdkit_pi_cc_word7((unsigned)(128.0f*k[6]));
 out[8]=mdkit_pi_cc_word8((unsigned)(128.0f*k[7]));
}


static void controls(mdkit_pi_cc*s){uint32_t packet[9];mdkit_pi_cc_packet(s->clock.current,packet);for(unsigned i=0;i<9;++i)s->storage.state_words[i+1]=packet[i];}
static void init(void*state){mdkit_pi_cc*s=state;
  /* Reset caller-owned storage, including clock and filter history. */
  for (unsigned i = 0; i < sizeof *s; ++i) ((unsigned char *)s)[i] = 0;
metal_reset(s);s->machine=71;controls(s);metal_activate(s);}
static void target(void*state,unsigned i,unsigned value){mdkit_events_target(&((mdkit_pi_cc*)state)->events,8,i,value);}
static void trigger(void*state){((mdkit_pi_cc*)state)->events.trigger=1;}
static void activate_note(mdkit_pi_cc*s){
 s->storage.voice.close_envelope=s->storage.voice.hardness;s->storage.voice.close_decay=mdkit_acc_to_word(mdkit_sub(mdkit_from24(0x7fffff),mdkit_from24(s->storage.voice.hardness)));
 s->storage.voice.attack_gate=s->storage.voice.window_budget=0;s->storage.voice.ring_amplitude=s->storage.voice.gate=0x7fffff;
 s->storage.voice.excitation_count=200;s->storage.voice.excitation_cursor=0x14a000;
}
static void block(mdkit_pi_cc*s){
 s->storage.voice.ring_amplitude=metal_product(s->storage.voice.ring_amplitude,s->storage.voice.grab);
 uint32_t old=s->storage.voice.close_envelope;s->storage.voice.close_envelope=metal_product(old,s->storage.voice.close_decay);
 s->storage.voice.amplitude=mdkit_acc_to_word(mdkit_add(mdkit_from24(s->storage.voice.amplitude),mdkit_from24(old)));
 if(s->storage.voice.gate)s->storage.voice.close_memory=s->storage.voice.amplitude;
 metal_noise(s,16);metal_excitation(s,s->storage.voice.impulse_step,s->storage.voice.excitation_threshold);metal_body_filters(s);
 uint32_t step=metal_product(s->storage.voice.gate?0x733333:s->storage.voice.pitch,0x49);
 uint32_t gain=mdkit_s24(s->storage.voice.hardness)>=0x19999a?metal_product(s->storage.voice.hardness,0x19999a):0;
 metal_window(s,step,gain,0);metal_amplitude(s);metal_coefficients(s);metal_coupling(s);
 gain=mdkit_s24(s->storage.voice.hardness)>0x19999a?s->storage.voice.hardness-0x19999a:0;
 metal_output(s,gain);
}
static void render(void*state,float*out,unsigned frames){mdkit_pi_cc*s=state;
 while(frames){if(s->pos==32){unsigned event=mdkit_events_boundary(&s->clock,&s->events,0);
  if(event&MDKIT_PI_PUBLISH)controls(s);if(event&MDKIT_PI_FIRST)metal_activate(s);if(event&MDKIT_PI_TRIGGER)activate_note(s);
  block(s);for(unsigned i=0;i<32;++i)s->block[i]=s->fraction[0x100+i];s->pos=0;}
  unsigned n=32-s->pos;if(n>frames)n=frames;
  for(unsigned i=0;i<n;++i)out[i] = s->events.triggered ? (float)mdkit_s24(s->block[s->pos + i]) / 8388608.0f : 0.0f;
  s->pos+=n;frames-=n;out+=n;}
}
static const char*const names[]={"PTCH","DEC","HARD","RING","AG","AU","BR","GRAB"};
const mdkit_voice mdkit_voice_pi_cc={"PI-CC",71,8,names,sizeof(mdkit_pi_cc),init,target,trigger,render, mdkit_pi_cc_defaults
};
