/* SPDX-License-Identifier: CC0-1.0
 * TRX-RS standalone synthesis voice.
 */
#include "mdkit_trx_rs.h"

/* ---- Default controls --------------------------------------------------- */
/* Active controls are in descriptor order; unused clock slots stay at 64. */
static const unsigned char mdkit_trx_rs_defaults[MDKIT_MAX_CONTROLS] = {
    50, 0, 50, 64, 64, 64, 64, 64
};
#define trx_rs_MD_MASK24 UINT32_C(0xffffff)
#define trx_rs_MD_MASK48 UINT64_C(0xffffffffffff)
#define trx_rs_MD_MASK56 UINT64_C(0xffffffffffffff)
#define trx_rs_MD_VOICE_BLOCK 32u
enum { trx_rs_MD_TRX_PUBLISH=1,trx_rs_MD_TRX_TRIGGER=2,trx_rs_MD_TRX_FIRST=4 };
static inline int64_t trx_rs_md_signed_bits(uint64_t word, unsigned bits)
{
    const uint64_t sign = UINT64_C(1) << (bits - 1);
    const uint64_t mask = (UINT64_C(1) << bits) - 1;
    word &= mask;
    return (word & sign) ? (int64_t)word - (int64_t)(UINT64_C(1) << bits)
                         : (int64_t)word;
}
static inline int64_t trx_rs_md_s24(uint32_t w) { return trx_rs_md_signed_bits(w & trx_rs_MD_MASK24, 24); }
static inline int64_t trx_rs_md_s56(uint64_t a) { return trx_rs_md_signed_bits(a & trx_rs_MD_MASK56, 56); }
static inline uint32_t trx_rs_md_mid(uint64_t a) { return (uint32_t)((a >> 24) & trx_rs_MD_MASK24); }
static inline uint32_t trx_rs_md_low(uint64_t a) { return (uint32_t)(a & trx_rs_MD_MASK24); }
static inline uint64_t trx_rs_md_from24(uint32_t w)
{ return ((uint64_t)(trx_rs_md_s24(w) * (INT64_C(1) << 24))) & trx_rs_MD_MASK56; }
static inline uint64_t trx_rs_md_from48(uint64_t w)
{ return (uint64_t)trx_rs_md_signed_bits(w & trx_rs_MD_MASK48, 48) & trx_rs_MD_MASK56; }
static inline uint64_t trx_rs_md_to48(uint64_t a)
{
    int64_t v = trx_rs_md_s56(a);
    if(v < -INT64_C(0x800000000000)) v = -INT64_C(0x800000000000);
    if(v >  INT64_C(0x7fffffffffff)) v =  INT64_C(0x7fffffffffff);
    return (uint64_t)v & trx_rs_MD_MASK48;
}
static inline uint64_t trx_rs_md_setmid(uint64_t a, uint32_t w)
{ return (a & ~(UINT64_C(0xffffff) << 24)) | ((uint64_t)(w & trx_rs_MD_MASK24) << 24); }
static inline uint64_t trx_rs_md_add(uint64_t a, uint64_t b) { return (a + b) & trx_rs_MD_MASK56; }
static inline uint64_t trx_rs_md_sub(uint64_t a, uint64_t b) { return (a - b) & trx_rs_MD_MASK56; }
static inline uint64_t trx_rs_md_asl(uint64_t a, unsigned n) { return (a << n) & trx_rs_MD_MASK56; }
static inline uint64_t trx_rs_md_asr(uint64_t a, unsigned n)
{
    const int64_t v = trx_rs_md_s56(a);
    const int64_t sh = v >= 0 ? v / (INT64_C(1) << n)
        : -((-v + (INT64_C(1) << n) - 1) / (INT64_C(1) << n));
    return (uint64_t)sh & trx_rs_MD_MASK56;
}
static inline uint64_t trx_rs_md_mpy(uint32_t a, uint32_t b)
{ return (uint64_t)(trx_rs_md_s24(a) * trx_rs_md_s24(b) * 2) & trx_rs_MD_MASK56; }
static inline uint64_t trx_rs_md_mac(uint64_t acc, uint32_t x, uint32_t y)
{
    return (acc + trx_rs_md_mpy(x, y)) & trx_rs_MD_MASK56;
}
static inline uint64_t trx_rs_md_and_mid(uint64_t a, uint32_t mask)
{ return trx_rs_md_setmid(a, trx_rs_md_mid(a) & mask); }
static inline uint64_t trx_rs_md_pack48(uint32_t hi, uint32_t lo)
{ return ((uint64_t)(hi & trx_rs_MD_MASK24) << 24) | (lo & trx_rs_MD_MASK24); }
static inline uint32_t trx_rs_md_acc_to_word(uint64_t a)
{
    const int64_t v = trx_rs_md_s56(a);
    if(v < -(INT64_C(0x800000) << 24)) return UINT32_C(0x800000);
    if(v >  (INT64_C(0x7fffff) << 24)) return UINT32_C(0x7fffff);
    return trx_rs_md_mid(a);
}
static inline void trx_rs_md_control_clock_seed(mdkit_control_clock *trx_rs_c, unsigned count,
                                         const unsigned char *knobs)
{
    trx_rs_c->count = count;
    for (unsigned i = 0; i < count; ++i) {
        const float v = (float)knobs[i];
        trx_rs_c->target[i] = trx_rs_c->raw[i] = trx_rs_c->current[i] = trx_rs_c->pending[i] = v;
    }
    trx_rs_c->block = 0; trx_rs_c->next_update = 0; trx_rs_c->publish_block = 0; trx_rs_c->pending_valid = 0;
}
static inline void trx_rs_md_trx_events_init(mdkit_control_clock *trx_rs_c,mdkit_trx_rs_events *e)
{

    trx_rs_md_control_clock_seed(trx_rs_c,8,mdkit_trx_rs_defaults); trx_rs_c->next_update=2;
    e->targets=e->trigger=e->triggered=0;
    for(unsigned i=0;i<8;++i){e->queued[i]=mdkit_trx_rs_defaults[i];e->last_raw[i]=(unsigned)mdkit_trx_rs_defaults[i] * 128u;}
}
static inline void trx_rs_md_trx_events_target(mdkit_trx_rs_events *e,unsigned count,unsigned i,unsigned value)
{ if(i<count){e->queued[i]=value>127?127:value;e->targets|=1u<<i;} }
static inline unsigned trx_rs_md_trx_events_boundary(mdkit_control_clock *trx_rs_c,mdkit_trx_rs_events *e,int trigger_only)
{
    unsigned result=0;
    if(trx_rs_c->pending_valid && trx_rs_c->block>=trx_rs_c->publish_block){
        for(unsigned i=0;i<8;++i)trx_rs_c->current[i]=trx_rs_c->pending[i];
        trx_rs_c->pending_valid=0;result=trx_rs_MD_TRX_PUBLISH;
    }
    if(trx_rs_c->block==trx_rs_c->next_update){
        int changed=0;
        for(unsigned i=0;i<8;++i){
            uint32_t raw=(uint32_t)(128.0f*trx_rs_c->raw[i]);
            trx_rs_c->pending[i]=trx_rs_c->raw[i];changed|=raw!=e->last_raw[i];e->last_raw[i]=raw;
            trx_rs_c->raw[i]=(float)((3*raw+128*(uint32_t)trx_rs_c->target[i])/4)/128.0f;
        }
        trx_rs_c->pending_valid=changed&&!trigger_only;trx_rs_c->publish_block=trx_rs_c->block+1;trx_rs_c->next_update+=12;
    }
    for(unsigned i=0;i<8;++i)if(e->targets&(1u<<i))trx_rs_c->target[i]=(float)e->queued[i];
    e->targets=0;
    if(e->trigger){
        for(unsigned i=0;i<8;++i)trx_rs_c->current[i]=trx_rs_c->raw[i];
        result=trx_rs_MD_TRX_PUBLISH|trx_rs_MD_TRX_TRIGGER|(e->triggered?0:trx_rs_MD_TRX_FIRST);
        e->trigger=0;e->triggered=1;
    }
    ++trx_rs_c->block;return result;
}
static uint32_t trx_rs_local_sine(unsigned index) {
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


static inline double trx_rs_trx_model_exp(double x);
static uint32_t trx_rs_rs_decay_curve(unsigned i){
 const double x=(double)i/159.0;
static const double trx_rs_c[8]={9.5593996693582977,-3.7372223907502309,0.071772495030229908,-0.33906053426869309,0.6824594180045066,-0.62641433952582226,0.2218761031047104,-0.0069215093934261399};
double y=trx_rs_c[7];for(unsigned j=7;j>0;--j)y=y*x+trx_rs_c[j-1];
 return (uint32_t)(8388608.0-trx_rs_trx_model_exp(y)+0.5)&0xffffffu;
}
static void trx_rs_trx_rs_packet(const float k[8],uint32_t p[4]){
 unsigned r[3];for(unsigned i=0;i<3;++i)r[i]=(unsigned)(128.0f*k[i]);
 p[0]=trx_rs_rs_decay_curve((160u*r[1])>>14);
 p[1]=4096u+(r[0]>>2);
 p[2]=(8388607u-32u*r[2])&0xffffffu;
 p[3]=16u*r[2];
}



#ifndef TRX_COMPACT_MODEL_MATH
#define TRX_COMPACT_MODEL_MATH
static inline double trx_rs_trx_model_exp(double x) {
    const double ln2=0.69314718055994530942;
    int n=(int)(x/ln2+(x<0?-0.5:0.5));
    double r=x-n*ln2,term=1,sum=1;
    for(unsigned k=1;k<=18;++k) { term*=r/k;sum+=term; }
    while(n>0) {sum*=2;--n;}
    while(n<0) {sum*=0.5;++n;}
    return sum;
}
static inline double trx_rs_trx_model_cos(double x) {
    double term=1,sum=1;
    for(unsigned k=1;k<=18;++k) {term*=-x*x/((2*k-1)*(2*k));sum+=term;}
    return sum;
}
static inline long long trx_model_round(double x) {return (long long)(x+(x<0?-0.5:0.5));}
static inline uint32_t trx_rs_trx_model_word(double v) {
    if (v > 8388607.0) v = 8388607.0;
    if (v < -8388608.0) v = -8388608.0;
    return (uint32_t)(int32_t)trx_model_round(v) & 0xffffffu;
}
static inline double trx_rs_trx_model_cheb(const double *trx_rs_c,unsigned n,double x) {
    double b1=0,b2=0;
    for(unsigned k=n;k>1;--k) { double b=2*x*b1-b2+trx_rs_c[k-1]; b2=b1;b1=b; }
    return x*b1-b2+trx_rs_c[0];
}
#endif
static const double trx_rs_trx_model_offset_a_c[30]={-232.399680398,153.649305252,-56.7606534091,23.5448649658,-10.6578480114,4.79767127404,-81.056751598,22.7077837039,-2.96244673295,0.299532376803,0.0532892400568,-0.0105212089343,-39.1668372957,14.9894597998,-2.21767366486,0.405478971463,-0.0117738691504,0.138342962517,-15.9882689259,7.39288842873,-1.35951208431,0.190096146761,0.00637058241878,-0.116429618127,-5.5138140884,3.0706297243,-0.547032748071,0.15553066531,0.0144368154093,-0.0443455945788};
static const double trx_rs_trx_model_offset_b_c[13]={-357.643862343,-265.373321533,59.3405978631,-17.7956076723,5.97881763422,-2.11982610249,0.784178870543,-0.267558388544,0.0889657720231,0.0650633677486,0.00493977326511,-0.0123225741715,-0.021290158449};
static inline int trx_model_offset(unsigned i) {
    if(i>=128) return (int)trx_model_round(trx_rs_trx_model_cheb(trx_rs_trx_model_offset_b_c,13,2.0*(i-128)/127.0-1.0));
    unsigned part=i<8?0:i<16?1:i<32?2:i<64?3:4;
    static const unsigned trx_rs_bounds[]={0,8,16,32,64,128};
    unsigned lo=trx_rs_bounds[part],hi=trx_rs_bounds[part+1];
    return (int)trx_model_round(trx_rs_trx_model_cheb(trx_rs_trx_model_offset_a_c+part*6,6,2.0*(i-lo)/(hi-lo-1)-1.0));
}
static inline uint32_t trx_rs_trx_model_memory(unsigned i) {
    if(i<256) return (uint32_t)trx_model_offset(i)&0xffffffu;
    unsigned x=i<4352?i-256:i-4352;
    double t=trx_rs_trx_model_exp(0.003372927270490704*x+(-12.768360569597025));
    return trx_rs_trx_model_word(5931641.6632877039*(i<4352?trx_rs_trx_model_cos(t):trx_rs_trx_model_exp(-t)));
}

static uint32_t trx_rs_trx_filter_memory(unsigned i){return trx_rs_trx_model_memory(i);}


static double trx_rs_rs_attack_sin(double x){
 const double pi=3.1415926535897932384626433832795;
 if(x>pi)x-=2*pi;
 if(x>pi/2)x=pi-x;else if(x< -pi/2)x=-pi-x;
 double t=x,sum=x;for(unsigned k=1;k<=9;++k){t*= -x*x/((2*k)*(double)(2*k+1));sum+=t;}return sum;
}
static uint32_t trx_rs_trx_rs_attack(unsigned i){
 if(i>=768)return 0;
static const double trx_rs_c[255]={-0.000891254800889,-0.00168620959926,-0.0014389584061,-0.00192723639014,-0.00215738834698,-0.00371536825324,-0.00824702160434,-0.00113565648188,-0.155079800738,0.0691050893854,0.00534260201052,0.0271467991615,0.0174011800081,0.0181016579488,0.00578524023477,0.027372090383,-0.106010586319,0.0374781144989,0.0500078311379,0.0359221659187,0.0223706711144,0.0203062881652,0.0292479968112,0.0317558812875,0.0750108255292,0.0447780936424,-0.0341114174624,-0.0197925650472,0.00902244404923,0.00138140137731,0.00675103146922,0.00302324343378,-0.0433600430653,0.0525761345676,0.065961282678,0.0266211608329,0.00568853485642,0.00814790620931,0.00292145852404,0.019744360022,0.0107479980701,0.00254906188705,-0.0253413014904,-0.0438230614345,-0.00422985346313,0.00485242308778,0.00831833661939,-0.00570686474392,0.0050244508755,-0.00441247102069,-0.0472248469214,-0.0124093225683,0.00709957985061,-0.00290444604407,-0.00137104844862,0.00408635055137,-0.00168494977902,0.00660195194965,0.00969024258432,-0.0336463399433,-0.0259876458758,0.00649654804053,-0.000332275812458,-0.00723459182008,-0.00766647503028,-0.0144491881971,-0.0287940707799,-0.00534894287455,0.0233583965882,0.0120033948154,0.0091992595986,-0.000306640315182,0.00224844377647,-0.00169949235134,-0.0215747886489,-0.0134818362401,-0.00155070487016,0.0155586958774,0.00745012121987,-0.00128543522717,0.00219071913606,-0.000750534595521,0.00345444713417,0.00105500026848,-0.00444535624016,-0.000873316393652,0.00084102077858,-0.00121487199003,-0.00704586095813,-0.00568296901198,-0.00982716464542,-0.00708306708475,0.00738018285506,0.0132451595129,0.00749639337121,-0.00494709854992,-0.00157818175204,-0.00208457625232,-0.0055520899455,0.00140208880323,-0.000652631335075,-0.00584639911888,-0.00109914909268,-0.00291124452525,-0.00148823466826,-0.00603198309871,-0.0002885922378,-0.00430983564013,-0.00885394859749,-0.000323796412973,0.00490282320629,0.00251754520388,-0.00419343133241,-0.00173633104377,-0.000232562417113,0.00026444238319,0.00457976909726,0.00158181619833,-0.00196946890089,-0.00464408771013,-0.00155210847998,-0.00382360003086,-0.00561172838932,-0.000732923293554,-0.00313938692712,-0.00582854877318,0.0015464485324,0.0052179823582,0.000491660053084,0.00101364056101,0.00251849807791,0.00331087439266,0.00723002030787,0.00939291811848,0.0217169347707,0.0587032644383,-0.121292566229,-0.0160095729715,-0.0265033973176,-0.00600825130592,-0.00603435134811,0.00563023292727,0.0165564763612,0.0146924250933,-0.107408776165,-0.0314220748935,0.00258336549352,-0.00596613371909,-0.00944764532656,-0.00773015508378,-0.0186175679594,0.0078395664553,0.0700440181909,0.0550232379624,-0.00857344913748,0.00105807409076,0.00798130960773,-0.00197131165711,0.0223985594063,-0.0229362845069,-0.0651932057234,0.0287719134025,0.0329232809148,0.0341028204751,0.0185023392485,0.0188104358392,0.013477768913,0.0366365214779,0.0343984696192,0.0463748016064,0.00324150933026,-0.014225725109,0.000788295646951,0.00968948110041,0.0168260788885,0.00725981166238,0.0408582150102,0.00960038470074,-0.0239187115742,-0.00433857518872,0.00605331178875,-0.00289127496246,0.00547560734312,0.00380733650034,0.00287202236365,0.0275990378068,0.0274265783532,-0.0199217154842,-0.00769238038466,0.00667663448061,0.00618254118286,0.00266758780999,0.0117237042515,-0.00849231845886,-0.0352707286567,-0.0132060452095,0.00453559304807,0.00454006222665,0.0104776105931,0.00680645160025,0.0177929282385,0.00598352535638,-0.0100436108494,-0.0164200546006,-0.00856344535765,0.0101206381895,0.00380834319571,0.00267554673513,0.00287044587473,0.0012937212645,0.00806876517544,0.00152141704147,0.00226995298168,0.00413202100631,0.00782565103316,0.0046136417871,0.00204675345665,0.000573588438856,-0.0105491577988,-0.0116819501584,0.000297399294153,0.010903236206,0.00911482667931,0.00182803701658,0.00642766070994,-3.40057358309e-05,-0.00246882427493,0.00877661978951,0.00344314058692,0.00167256176848,0.00367222843062,0.00227756389567,0.00120941574907,-0.00118668816446,0.00650100043056,-0.003112300805,-0.00766753477787,-0.00157875217276,0.00464734882378,0.00345606781284,-0.00122018633479,0.000246630108972,-0.00206776381699,0.00116285200358,0.00548753022846,0.00602514619989,0.00260574147136,0.00111347976875,0.00256623245041,-0.00111222064159,-1.90846000241e-05,0.00227609196548,-0.00250948766413,-0.00611685367437,0.00113654168221};
 const double theta=6.283185307179586476925286766559*(double)i/768.0;
 const double sn=trx_rs_rs_attack_sin(theta),cs=trx_rs_rs_attack_sin(theta+1.57079632679489661923);
 double s=sn,q=cs,y=trx_rs_c[0];
 for(unsigned h=0;h<127;++h){y+=trx_rs_c[1+h]*q+trx_rs_c[1+127+h]*s;double old=q;q=q*cs-s*sn;s=s*cs+old*sn;}
 double w=y*8388608.0;if(w>8388607.0)w=8388607.0;if(w< -8388608.0)w=-8388608.0;
 return (uint32_t)(int32_t)(w+(w<0?-0.5:0.5))&0xffffffu;
}






static void trx_rs_rs_controls(mdkit_trx_rs*s)
{uint32_t p[4];trx_rs_trx_rs_packet(s->clock.current,p);s->decay=p[0];s->pitch=p[1];s->threshold=p[2];s->bias=p[3];}
static void trx_rs_rs_init(void*state)
{
 mdkit_trx_rs*s=state;trx_rs_md_trx_events_init(&s->clock,&s->events);s->pos=0;
 s->amplitude=s->clip_carry=s->attack_cursor=s->phase_lo=0;s->phase_hi=0x2000;
 s->first_hi=s->first_low=s->first_delay=s->second_hi=s->second_low=s->second_delay=0;
 s->input_a=s->input_b=0;
 for(unsigned i=0;i<128;++i)s->workspace[i]=s->fraction[i]=0;
 for(unsigned i=0;i<32;++i)s->block[i]=0;
 trx_rs_rs_controls(s);
}
static void trx_rs_rs_target(void*state,unsigned i,unsigned value){trx_rs_md_trx_events_target(&((mdkit_trx_rs*)state)->events,3,i,value);}
static void trx_rs_rs_trigger(void*state){((mdkit_trx_rs*)state)->events.trigger=1;}
static void trx_rs_rs_store_long(mdkit_trx_rs*s,unsigned index,uint64_t accumulator)
{uint64_t word=trx_rs_md_to48(accumulator);s->workspace[index]=trx_rs_md_mid(word);s->fraction[index]=trx_rs_md_low(word);}
static void trx_rs_rs_filter(mdkit_trx_rs*s)
{
 uint32_t*w=s->workspace;
 uint32_t norm_index=(0x200+trx_rs_trx_filter_memory(0))&trx_rs_MD_MASK24;
 uint32_t coef_index=(0x200+trx_rs_trx_filter_memory(0x80))&trx_rs_MD_MASK24;
 uint32_t resonance=trx_rs_trx_filter_memory(0x1900+coef_index);
 uint64_t coupling=trx_rs_md_asl(trx_rs_md_mpy(resonance,trx_rs_trx_filter_memory(0x900+norm_index)),1);
 uint32_t feedback=trx_rs_md_acc_to_word(trx_rs_md_add(coupling,trx_rs_md_from24(0xc00000)));
 uint32_t square=trx_rs_md_acc_to_word(trx_rs_md_asl(trx_rs_md_sub(0,trx_rs_md_mpy(resonance,resonance)),1));
 w[66]=s->first_delay;w[67]=s->first_hi;
 w[32]=s->second_delay;w[33]=s->second_hi;
 w[94]=s->input_b;w[95]=s->input_a;
 uint64_t first=trx_rs_md_from48(trx_rs_md_pack48(s->first_hi,s->first_low));
 uint64_t second=trx_rs_md_from48(trx_rs_md_pack48(s->second_hi,s->second_low));
 first=trx_rs_md_mac(first,square,w[66]);
 first=trx_rs_md_mac(first,w[67],feedback);
 first=trx_rs_md_mac(first,w[67],feedback);
 unsigned history=67,input=95,output=33;
 uint32_t source=w[94];
 for(unsigned i=0;i<32;++i){
  first=trx_rs_md_mac(first,source,0x400000);
  first=trx_rs_md_mac(first,w[input++],0x800000);
  first=trx_rs_md_mac(first,w[input--],0x400000);
  --output;uint32_t delayed=w[output++];
  second=trx_rs_md_mac(second,square,delayed);
  uint32_t adjacent=w[output++];
  second=trx_rs_md_mac(second,feedback,adjacent);
  uint32_t first_word=trx_rs_md_mid(first);
  second=trx_rs_md_mac(second,feedback,adjacent);
  second=trx_rs_md_mac(second,first_word,0x400000);
  second=trx_rs_md_mac(second,w[history],0x800000);history=history==66?67:66;
  second=trx_rs_md_mac(second,w[history],0x400000);
  trx_rs_rs_store_long(s,history,first);history=history==66?67:66;
  first=trx_rs_md_mac(first,square,w[history]);history=history==66?67:66;
  uint32_t new_history=w[history];
  first=trx_rs_md_mac(first,new_history,feedback);
  trx_rs_rs_store_long(s,output,second);
  first=trx_rs_md_mac(first,new_history,feedback);
  source=w[input++];
 }
 s->first_hi=w[history];s->first_low=s->fraction[history];history=history==66?67:66;
 s->first_delay=w[history];
 s->second_hi=w[output];s->second_low=s->fraction[output];--output;
 s->second_delay=w[output];s->input_a=w[input];s->input_b=w[input-1];
}
static void trx_rs_rs_block(mdkit_trx_rs*s)
{
 uint32_t*w=s->workspace;
 uint64_t phase=trx_rs_md_from48(trx_rs_md_pack48(s->phase_hi,s->phase_lo));
 uint64_t increment=trx_rs_md_asr(trx_rs_md_from24(s->pitch),5);
 for(unsigned i=0;i<32;++i){phase=trx_rs_md_and_mid(phase,0x7fff);unsigned index=trx_rs_md_mid(phase);phase=trx_rs_md_add(phase,increment);w[i]=trx_rs_local_sine(index);}
 s->phase_hi=trx_rs_md_mid(phase);s->phase_lo=trx_rs_md_low(phase);
 uint64_t carry=trx_rs_md_from24(s->clip_carry);
 for(unsigned i=0;i<32;++i){
  uint64_t value=trx_rs_md_from24(w[i]);
  if(trx_rs_md_s56(value)>=trx_rs_md_s56(trx_rs_md_from24(s->threshold))){value=trx_rs_md_sub(trx_rs_md_from24(0x7eb852),carry);carry=trx_rs_md_add(carry,trx_rs_md_from24(s->bias));}
  else{
   value=trx_rs_md_sub(0,value);
   if(trx_rs_md_s56(value)>=trx_rs_md_s56(trx_rs_md_from24(s->threshold))){value=trx_rs_md_add(trx_rs_md_from24(0x8147ae),carry);carry=trx_rs_md_add(carry,trx_rs_md_from24(s->bias));}
   else{value=trx_rs_md_sub(0,value);carry=0;}
  }
  w[i]=trx_rs_md_acc_to_word(value);
 }
 s->clip_carry=trx_rs_md_acc_to_word(carry);
 for(unsigned i=0;i<32;++i)w[i]=trx_rs_md_acc_to_word(trx_rs_md_mpy(w[i],0x400000));
 unsigned attack=s->attack_cursor;
 if(attack<0x300)s->attack_cursor+=32;
 for(unsigned i=0;i<32;++i){
  uint64_t mixed=trx_rs_md_from24(w[i]);if(attack<0x300)mixed=trx_rs_md_add(mixed,trx_rs_md_from24(trx_rs_trx_rs_attack(attack+i)));
  w[96+i]=trx_rs_md_acc_to_word(trx_rs_md_mpy(s->amplitude,trx_rs_md_acc_to_word(mixed)));
  s->amplitude=trx_rs_md_acc_to_word(trx_rs_md_mpy(s->amplitude,s->decay));
 }
 trx_rs_rs_filter(s);for(unsigned i=0;i<32;++i)s->block[i]=trx_rs_md_acc_to_word(trx_rs_md_asl(trx_rs_md_from24(w[32+i]),1));
}
static void trx_rs_rs_render(void*state,float*out,unsigned frames)
{
 mdkit_trx_rs*s=state;
 while(frames){
  if(!s->pos){unsigned event=trx_rs_md_trx_events_boundary(&s->clock,&s->events,0);
   if(event&trx_rs_MD_TRX_PUBLISH)trx_rs_rs_controls(s);
   if(event&trx_rs_MD_TRX_FIRST){s->amplitude=0;s->phase_hi=0x2000;s->phase_lo=0;}
   if(event&trx_rs_MD_TRX_TRIGGER){
    s->amplitude=trx_rs_md_acc_to_word(trx_rs_md_add(trx_rs_md_from24(0x50c335),trx_rs_md_from24(s->amplitude)));
    s->attack_cursor=s->clip_carry=0;
    s->first_hi=s->first_delay=s->second_hi=s->second_delay=s->input_a=s->input_b=0;
   }
   trx_rs_rs_block(s);
  }
  unsigned n=32-s->pos;if(n>frames)n=frames;
  for(unsigned i=0;i<n;++i)out[i]=(float)trx_rs_md_s24(s->block[s->pos+i])/8388608.0f;
  out+=n;frames-=n;s->pos=(s->pos+n)&31;
 }
}
static const char*const trx_rs_names[]={"PTCH","DEC","DIST"};
const mdkit_voice mdkit_voice_trx_rs={"TRX-RS",20,3,trx_rs_names,sizeof(mdkit_trx_rs),trx_rs_rs_init,trx_rs_rs_target,trx_rs_rs_trigger,trx_rs_rs_render, mdkit_trx_rs_defaults
};
