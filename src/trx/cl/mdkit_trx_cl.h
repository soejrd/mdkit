/* SPDX-License-Identifier: CC0-1.0
 * TRX-CL standalone synthesis voice.
 */
#ifndef MDKIT_TRX_CL_H
#define MDKIT_TRX_CL_H
#include <stdint.h>
#include "mdkit_voice.h"
typedef struct { uint32_t queued[8],last_raw[8];unsigned targets,trigger,triggered;} mdkit_trx_cl_events;



typedef struct mdkit_trx_cl {
    mdkit_control_clock clock;
    mdkit_trx_cl_events events;
    unsigned pos;
    uint32_t decay,fill_count,pitch_a,pitch_b,filter_offset,filter_shape,dual_gain,click;
    uint32_t amplitude,phase_a_hi,phase_a_lo,phase_b_hi,phase_b_lo;
    uint32_t input_a,input_b,output_a,output_b,delay_a,delay_b;
    uint32_t workspace[128],block[32];
} mdkit_trx_cl;
extern const mdkit_voice mdkit_voice_trx_cl;


#endif
