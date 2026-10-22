/* SPDX-License-Identifier: CC0-1.0
 * TRX-SD standalone synthesis voice.
 */
#ifndef MDKIT_TRX_SD_H
#define MDKIT_TRX_SD_H
#include <stdint.h>
#include "mdkit_voice.h"


enum { MDKIT_TRX_SD_PTCH, MDKIT_TRX_SD_DEC, MDKIT_TRX_SD_BUMP, MDKIT_TRX_SD_BENV,
       MDKIT_TRX_SD_SNAP, MDKIT_TRX_SD_TONE, MDKIT_TRX_SD_TUNE, MDKIT_TRX_SD_CLIP,
       MDKIT_TRX_SD_CONTROLS };
typedef struct mdkit_trx_sd {
    mdkit_control_clock clock;
    unsigned pos, pending_trigger, triggered, target_mask;
    uint32_t queued_targets[8], last_raw[8];
    uint32_t decay, noise_decay, tone_a, tone_b, snap, snap_boost;
    uint32_t pitch_a, pitch_b, shape_decay, bump_step, filter_index, bump_decay;
    uint32_t body_amplitude, noise_amplitude, shape, bump, attack_cursor;
    uint64_t phase_a, phase_b;
    uint32_t noise_a, noise_b, filter_a, filter_b, filter_c, filter_d;
    uint32_t last_output, workspace[96], block[32];
} mdkit_trx_sd;
extern const mdkit_voice mdkit_voice_trx_sd;


#endif
