/* SPDX-License-Identifier: CC0-1.0
 * TRX-CH standalone synthesis voice.
 */
#ifndef MDKIT_TRX_CH_H
#define MDKIT_TRX_CH_H
#include <stdint.h>
#include "mdkit_voice.h"


typedef struct {
 mdkit_control_clock clock;
 unsigned machine,pos,pending_trigger,triggered,target_mask;
 unsigned queued_targets[8],last_raw[8];
 uint32_t linear_step,decay,lowpass_index,lowpass_resonance,noise_gain;
 uint32_t highpass_index,highpass_resonance,metal_gain,hold_blocks,fade_blocks;
 uint32_t latched_metal_gain;
 uint32_t amplitude,noise_a,noise_b,cursors[6],resonant_history[4],difference_history[2][4];
 uint32_t workspace[128],block[32];
} mdkit_trx_ch;

extern const mdkit_voice mdkit_voice_trx_ch;



#endif
