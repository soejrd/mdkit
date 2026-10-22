/* SPDX-License-Identifier: CC0-1.0
 * TRX-CY standalone synthesis voice.
 */
#ifndef MDKIT_TRX_CY_H
#define MDKIT_TRX_CY_H
#include <stdint.h>
#include "mdkit_voice.h"
typedef struct { uint32_t queued[8],last_raw[8];unsigned targets,trigger,triggered;} mdkit_trx_cy_events;



typedef struct {
 mdkit_control_clock clock;mdkit_trx_cy_events events;unsigned pos;
 uint32_t decay,metal_gains[6],top_index,size_index[2],top_gain,peak,latched_peak,body_decay;
 uint32_t amplitude[2],cursors[6],difference_history[2][6],resonant_history[2][4];
 uint32_t workspace[160],block[32];
} mdkit_trx_cy;

extern const mdkit_voice mdkit_voice_trx_cy;


#endif
