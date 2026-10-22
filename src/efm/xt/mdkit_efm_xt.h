/* SPDX-License-Identifier: CC0-1.0
 * mdkit_efm_xt.h - EFM-XT (machine 34) standalone voice.
 */
#ifndef MDKIT_EFM_XT_H
#define MDKIT_EFM_XT_H
#include "mdkit_voice.h"
#include <stdint.h>

enum {
    MDKIT_EFM_XT_PTCH = 0, MDKIT_EFM_XT_DEC, MDKIT_EFM_XT_RAMP,
    MDKIT_EFM_XT_RDEC, MDKIT_EFM_XT_MOD, MDKIT_EFM_XT_MFRQ,
    MDKIT_EFM_XT_MDEC, MDKIT_EFM_XT_CLIC, MDKIT_EFM_XT_CONTROLS
};

typedef struct mdkit_efm_xt {
    mdkit_control_clock clock;
    unsigned pos;
    uint32_t slot[9];
    uint32_t tail_coef;
    uint32_t fb, op1_out, op2_out;
    uint64_t acc1, acc2, acc3;
    uint32_t tail_hist, tail_delay;
    uint32_t block[32];
    unsigned have_block;
    /* Derived coefficient tables, built once per instance by init. */
    uint32_t pitch[128];
    uint32_t decay[326];
} mdkit_efm_xt;

extern const mdkit_voice mdkit_voice_efm_xt;

#endif /* MDKIT_EFM_XT_H */
