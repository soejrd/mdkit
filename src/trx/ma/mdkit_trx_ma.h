/* SPDX-License-Identifier: CC0-1.0
 * TRX-MA standalone synthesis voice.
 */
#ifndef MDKIT_TRX_MA_H
#define MDKIT_TRX_MA_H
#include <stdint.h>
#include "mdkit_voice.h"


typedef struct mdkit_trx_ma {
 mdkit_control_clock clock;
 unsigned pos,pending_trigger,triggered,target_mask;
 uint32_t queued_targets[8],last_raw[8],parameters[11];
 uint32_t noise_a,noise_b,attack_blocks,sustain_blocks,release_blocks,envelope;
 uint32_t rattle_growth,rattle_floor,rattle_rate,room_amplitude,rattle_attack,rattle_decay;
 uint32_t source_filter[6],rattle_filter[6],tone_filter[6];
 uint32_t workspace[160],auxiliary[160],block[32];
} mdkit_trx_ma;
extern const mdkit_voice mdkit_voice_trx_ma;


#endif
