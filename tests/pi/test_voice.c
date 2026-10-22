/* SPDX-License-Identifier: CC0-1.0 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include MDKIT_TEST_VOICE_HEADER
static unsigned failures;
#define CHECK(c,m) do { if(!(c)){printf("FAIL %s\n",m);++failures;} } while(0)
static double peak(const float*x,unsigned n){double p=0;for(unsigned i=0;i<n;++i)if(fabs(x[i])>p)p=fabs(x[i]);return p;}
static double rms(const float*x,unsigned n){double p=0;for(unsigned i=0;i<n;++i)p+=(double)x[i]*x[i];return sqrt(p/n);}
static int bounded(const float*x,unsigned n){for(unsigned i=0;i<n;++i)if(!isfinite(x[i])||fabs(x[i])>1.000001)return 0;return 1;}
static void chunks(const mdkit_voice*v,void*s,float*x,unsigned n){static const unsigned sizes[]={1,7,31,257,32,63,511};unsigned k=0;while(n){unsigned q=sizes[k++%7];if(q>n)q=n;v->render(s,x,q);x+=q;n-=q;}}
static void event_render(const mdkit_voice*v,void*s,float*x,unsigned n){
 unsigned pos=0;while(pos<n){if(pos==0)v->trigger(s);if(pos==1024)v->set_control(s,0,0);if(pos==4096)v->trigger(s);if(pos==8192)v->set_control(s,0,127);if(pos==12288)v->trigger(s);
 unsigned q=pos<1024?1024-pos:pos<4096?4096-pos:pos<8192?8192-pos:pos<12288?12288-pos:n-pos;if(q>n-pos)q=n-pos;chunks(v,s,x+pos,q);pos+=q;}}
int main(int argc,char**argv){const mdkit_voice*v=&VOICE_SYMBOL;const unsigned n=16384;
 void*a=calloc(1,v->state_bytes),*b=calloc(1,v->state_bytes),*c=calloc(1,v->state_bytes);
 float*x=calloc(n,sizeof(float)),*y=calloc(n,sizeof(float)),*z=calloc(n,sizeof(float));
 float*low=calloc(80*n,sizeof(float));if(!a||!b||!c||!x||!y||!z||!low)return 2;
 memset(a,0xa5,v->state_bytes);memset(b,0x5a,v->state_bytes);
 v->init(a);v->init(b);v->trigger(a);v->trigger(b);v->render(a,x,n);chunks(v,b,y,n);
 CHECK(!memcmp(x,y,n*sizeof(float)),"init resets arbitrary caller storage");
 CHECK(v->control_count>0&&v->control_count<=8,"control count");CHECK(v->machine_id==VOICE_MACHINE_ID,"machine id");
 v->init(a);chunks(v,a,x,n);CHECK(peak(x,n)==0,"silence before trigger");
 v->init(a);v->trigger(a);v->render(a,x,n);CHECK(bounded(x,n),"finite bounded triggered output");CHECK(peak(x,n)>1e-7,"audible trigger");printf("default peak=%.9g rms=%.9g\n",peak(x,n),rms(x,n));
 v->init(b);v->trigger(b);chunks(v,b,y,n);CHECK(!memcmp(x,y,n*sizeof(float)),"render chunk invariance");
 v->init(a);v->init(b);v->init(c);v->trigger(a);v->trigger(b);v->trigger(c);v->render(a,x,n);
 for(unsigned i=0;i<n;){unsigned q=n-i>257?257:n-i;v->render(b,y+i,q);v->set_control(c,0,(i/257)%128);v->trigger(c);v->render(c,z,q);i+=q;}
 CHECK(!memcmp(x,y,n*sizeof(float)),"independent instance isolation");
 v->init(a);v->init(b);v->trigger(a);v->trigger(b);v->render(a,x,8192);v->render(b,y,8192);v->trigger(b);v->render(a,x,n);v->render(b,y,n);
 CHECK(bounded(y,n),"finite bounded retrigger");double delta=0;unsigned changes=0;for(unsigned i=0;i<n;++i){double d=fabs(x[i]-y[i]);if(d>delta)delta=d;if(d>1e-7)++changes;}CHECK(delta>1e-7,"retrigger changes signal");printf("retrigger maxdiff=%.9g changed=%u/%u\n",delta,changes,n);
 double endpoint_delta=0;for(unsigned edge=0;edge<2;++edge){v->init(a);v->set_control(a,0,edge?127:0);for(unsigned rest=44100;rest;){unsigned q=rest>n?n:rest;chunks(v,a,x,q);rest-=q;}v->trigger(a);double p=0;
 for(unsigned j=0;j<80;++j){chunks(v,a,x,n);if(!edge)memcpy(low+j*n,x,n*sizeof(float));else for(unsigned i=0;i<n;++i){double d=fabs(x[i]-low[j*n+i]);if(d>endpoint_delta)endpoint_delta=d;}CHECK(bounded(x,n),"finite bounded endpoint/tail");if(peak(x,n)>p)p=peak(x,n);if(j==0||j==2||j==13||j==26||j==79)printf("endpoint=%u time=%.3f rms=%.9g peak=%.9g\n",edge?127:0,(j+1)*n/44100.0,rms(x,n),peak(x,n));}
 CHECK(p>1e-7,"endpoint audible");}
 CHECK(endpoint_delta>1e-7,"settled first-control endpoints differ");printf("settled endpoint maxdiff=%.9g\n",endpoint_delta);
 for(unsigned control=1;control<v->control_count;++control)for(unsigned edge=0;edge<2;++edge){v->init(a);v->set_control(a,control,edge?127:0);for(unsigned rest=44100;rest;){unsigned q=rest>n?n:rest;chunks(v,a,x,q);rest-=q;}v->trigger(a);chunks(v,a,x,n);CHECK(bounded(x,n),"finite bounded additional-control endpoint");}
 printf("all %u control endpoint settings finite/bounded\n",2*v->control_count);
 v->init(a);v->trigger(a);for(unsigned j=0;j<27;++j){chunks(v,a,x,n);CHECK(bounded(x,n),"finite bounded default long tail");if(j==2||j==13||j==26)printf("default-tail time=%.3f rms=%.9g peak=%.9g\n",(j+1)*n/44100.0,rms(x,n),peak(x,n));}
 v->init(a);event_render(v,a,x,n);CHECK(bounded(x,n),"finite bounded control-event schedule");
 if(argc==3&&!strcmp(argv[1],"--render")){FILE*f=fopen(argv[2],"wb");if(!f)return 2;if(fwrite(x,sizeof(float),n,f)!=n)return 2;fclose(f);}
 printf("%s technical checks: %u failures; listening pending\n",v->name,failures);free(a);free(b);free(c);free(x);free(y);free(z);free(low);return failures?1:0;}
