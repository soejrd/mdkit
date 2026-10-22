/* SPDX-License-Identifier: CC0-1.0 */

#ifndef MDKIT_TRX_B2_H
#define MDKIT_TRX_B2_H
#include "mdkit_voice.h"
#include <stdint.h>

enum {
    MDKIT_TRX_B2_PTCH, MDKIT_TRX_B2_DEC, MDKIT_TRX_B2_RAMP, MDKIT_TRX_B2_HOLD,
    MDKIT_TRX_B2_TICK, MDKIT_TRX_B2_NOIS, MDKIT_TRX_B2_DIRT, MDKIT_TRX_B2_DIST,
    MDKIT_TRX_B2_CONTROLS
};

typedef struct mdkit_trx_b2 {
    mdkit_control_clock clock;
    unsigned pos;
    unsigned pending_trigger, triggered, target_mask;
    uint32_t queued_targets[8];
    uint32_t last_raw[8];
    uint32_t pitch, ramp_amount, decay, hold_decay, warp[4];
    uint32_t dirt_shift, dirt_bias, noise_level, tick_level;
    uint32_t ramp, phase, attack_step, amplitude, hold;
    uint32_t noise_a, noise_b, noise_amplitude, tick;
    uint32_t block[32];
} mdkit_trx_b2;

extern const mdkit_voice mdkit_voice_trx_b2;
#endif
