/* SPDX-License-Identifier: CC0-1.0 */

#ifndef MDKIT_TRX_XC_H
#define MDKIT_TRX_XC_H
#include "mdkit_voice.h"
#include <stdint.h>


enum {
    MDKIT_TRX_XC_PTCH = 0, MDKIT_TRX_XC_DEC, MDKIT_TRX_XC_RAMP, MDKIT_TRX_XC_RDEC,
    MDKIT_TRX_XC_DAMP,     MDKIT_TRX_XC_DIST, MDKIT_TRX_XC_DTYP,
    MDKIT_TRX_XC_CONTROLS
};

typedef struct xc_trx_events {
    uint32_t queued[8], last_raw[8];
    unsigned targets, trigger, triggered;
} xc_trx_events;
typedef struct mdkit_trx_xc {
    mdkit_control_clock clock;
    xc_trx_events events;
    unsigned pos;
    uint32_t decay, pitch, ramp_amount, fold_upper, fold_lower, fold_step, ramp_decay;
    uint32_t ramp;
    uint32_t stage;
    uint32_t amplitude;
    uint32_t attack_gain;
    uint32_t phase_hi, phase_lo;
    uint32_t out_prev;
    uint32_t dmp_a, dmp_b;
    uint32_t damp_seed;
    uint32_t delay[32];
    uint32_t block[32];
    unsigned have_block;
} mdkit_trx_xc;

extern const mdkit_voice mdkit_voice_trx_xc;

#endif
