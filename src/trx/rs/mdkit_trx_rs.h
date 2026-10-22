/* SPDX-License-Identifier: CC0-1.0
 * TRX-RS standalone synthesis voice.
 */
#ifndef MDKIT_TRX_RS_H
#define MDKIT_TRX_RS_H
#include <stdint.h>
#include "mdkit_voice.h"
typedef struct { uint32_t queued[8],last_raw[8];unsigned targets,trigger,triggered;} mdkit_trx_rs_events;



enum { MDKIT_TRX_RS_PTCH,MDKIT_TRX_RS_DEC,MDKIT_TRX_RS_DIST,MDKIT_TRX_RS_CONTROLS };
typedef struct mdkit_trx_rs {
    mdkit_control_clock clock;
    mdkit_trx_rs_events events;
    unsigned pos;
    uint32_t decay,pitch,threshold,bias;
    uint32_t amplitude,clip_carry,attack_cursor,phase_hi,phase_lo;
    uint32_t first_hi,first_low,first_delay,second_hi,second_low,second_delay;
    uint32_t input_a,input_b,workspace[128],fraction[128],block[32];
} mdkit_trx_rs;
extern const mdkit_voice mdkit_voice_trx_rs;


#endif
