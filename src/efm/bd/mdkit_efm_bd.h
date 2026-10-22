/* SPDX-License-Identifier: CC0-1.0
 * mdkit_efm_bd.h - EFM-BD standalone voice, machine 32.
 *
 * Caller-owned state and control indices. DSP behavior lives in the matching
 * .c file.
 */
#ifndef MDKIT_EFM_BD_H
#define MDKIT_EFM_BD_H
#include "mdkit_voice.h"

/* Control indices in the stock 0..127 knob order (also the browser order). */
enum {
    MDKIT_EFM_BD_PTCH = 0, MDKIT_EFM_BD_DEC, MDKIT_EFM_BD_RAMP, MDKIT_EFM_BD_RDEC,
    MDKIT_EFM_BD_MOD,      MDKIT_EFM_BD_MFRQ, MDKIT_EFM_BD_MDEC, MDKIT_EFM_BD_MFB,
    MDKIT_EFM_BD_CONTROLS
};

typedef struct mdkit_efm_bd {
    mdkit_control_clock clock;   /* smoothed/latched 0..127 control values */
    unsigned pos;             /* intra-block sample position, 0..31   */
    double ph1, ph2, mod;     /* phases, 32768.0 == one sine cycle    */
    double op1, op2, fb;      /* operator/feedback envelopes (fraction)*/
} mdkit_efm_bd;

extern const mdkit_voice mdkit_voice_efm_bd;

#endif /* MDKIT_EFM_BD_H */
