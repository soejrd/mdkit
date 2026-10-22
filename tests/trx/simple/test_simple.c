/* SPDX-License-Identifier: CC0-1.0 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mdkit_trx_xt.h"
#include "mdkit_trx_xc.h"
#include "mdkit_trx_b2.h"

static void render_sizes(const mdkit_voice *v, void *s, float *out, unsigned n)
{
    static const unsigned sizes[]={1,7,31,257,97};
    unsigned pos=0,j=0;
    while(pos<n){unsigned count=sizes[j++%5];if(count>n-pos)count=n-pos;v->render(s,out+pos,count);pos+=count;}
}
static int bounded(const float *x,unsigned n)
{for(unsigned i=0;i<n;++i)if(!isfinite(x[i])||fabsf(x[i])>1.0f)return 0;return 1;}
static double power(const float *x,unsigned n)
{double q=0;for(unsigned i=0;i<n;++i)q+=x[i]*x[i];return q/(double)n;}
int main(void)
{
    const mdkit_voice *voices[]={&mdkit_voice_trx_xt,&mdkit_voice_trx_xc,&mdkit_voice_trx_b2};
    float *a=malloc(88200*sizeof(float)),*b=malloc(88200*sizeof(float));
    if(!a||!b)return 2;
    for(unsigned k=0;k<3;++k){const mdkit_voice *v=voices[k];void *s=malloc(v->state_bytes),*t=malloc(v->state_bytes);if(!s||!t)return 2;
        memset(s,0xa5,v->state_bytes);v->init(s);render_sizes(v,s,a,8000);if(power(a,8000)!=0)return 1;
        v->init(s);v->trigger(s);v->render(s,a,88200);
        memset(t,0x5a,v->state_bytes);v->init(t);v->trigger(t);render_sizes(v,t,b,88200);
        if(memcmp(a,b,88200*sizeof(float))||!bounded(a,88200)||power(a,2000)<1e-8||power(a+80000,8200)>power(a,2000)*0.01)return 1;
        v->init(t);render_sizes(v,t,b,88200);if(power(b,88200)!=0)return 1;
        for(unsigned i=0;i<v->control_count;++i)for(unsigned extreme=0;extreme<2;++extreme){v->init(s);v->set_control(s,i,extreme?127:0);render_sizes(v,s,a,8000);v->trigger(s);render_sizes(v,s,a,88200);if(!bounded(a,88200))return 1;}
        v->init(s);v->trigger(s);render_sizes(v,s,a,1001);v->trigger(s);render_sizes(v,s,a,88200);if(!bounded(a,88200)||power(a,2000)<1e-8)return 1;
        for(unsigned extreme=0;extreme<2;++extreme){v->init(s);v->set_control(s,0,extreme?127:0);render_sizes(v,s,a+8000,8000);v->trigger(s);render_sizes(v,s,extreme?b:a,4000);}
        double diff=0;for(unsigned i=0;i<4000;++i)diff+=fabs((double)a[i]-b[i]);if(diff<1)return 1;
        printf("%s: link, initialized storage, idle/tail, bounded endpoints, isolated state, irregular sizes/retrigger, settled pitch OK\n",v->name);free(s);free(t);
    }
    free(a);free(b);return 0;
}
