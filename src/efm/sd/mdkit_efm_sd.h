/* SPDX-License-Identifier: CC0-1.0
 * mdkit_efm_sd.h - EFM-SD standalone voice, machine 33.
 *
 * Caller-owned state and control indices. DSP behavior lives in the matching
 * .c file.
 */
#ifndef MDKIT_EFM_SD_H
#define MDKIT_EFM_SD_H
#include "mdkit_voice.h"
#include <stdint.h>

/* Stock knob order (md_web.c extra_controls for machine 33). */
enum {
    MDKIT_EFM_SD_PTCH = 0, MDKIT_EFM_SD_DEC, MDKIT_EFM_SD_NOIS, MDKIT_EFM_SD_NDEC,
    MDKIT_EFM_SD_MOD,      MDKIT_EFM_SD_MFRQ, MDKIT_EFM_SD_MDEC, MDKIT_EFM_SD_HPF,
    MDKIT_EFM_SD_CONTROLS
};

/* Slot roles, named from efm_sd_dsp.c (X index = s->y index):
 *   x1  PTCH  operator-1 rate            x7  NOIS  noise-ring mix amount
 *   x2  DEC   operator-1 output decay    x8  NDEC  operator-2 output decay
 *   x3  MFRQ  phase-1 rate scale         x9  0x44aa20 fixed first-resonator coef
 *   x4  MOD   phase-2 modulation scale   x0a HPF   output-resonator coef
 *   x5  MDEC  feedback register decay
 *   x6  0x7a1200 fixed modulator index
 * Accumulators: L:b operator-1 phase, L:d operator-2 phase, L:12 modulator.
 * Registers: x0f feedback, x10 operator-1 out, x11 operator-2 out.
 * State: x14/x15 noise recurrence, x16/x17 first resonator, x18/x19 output
 * resonator. */
typedef struct mdkit_efm_sd {
    mdkit_control_clock clock;
    unsigned pos;

    uint32_t op1_rate;     /* x1  PTCH */
    uint32_t op1_decay;    /* x2  DEC  */
    uint32_t mod_rate;     /* x3  MFRQ */
    uint32_t mod_amount;   /* x4  MOD  */
    uint32_t fb_decay;     /* x5  MDEC */
    uint32_t mod_index;    /* x6  fixed 0x7a1200 */
    uint32_t noise_amount; /* x7  NOIS */
    uint32_t op2_decay;    /* x8  NDEC */
    uint32_t noise_coef;   /* x9  fixed 0x44aa20 */
    uint32_t tail_coef;    /* x0a HPF  */

    uint64_t l_b;          /* L:b  operator-1 phase accumulator */
    uint64_t l_d;          /* L:d  operator-2 phase accumulator */
    uint64_t l_12;         /* L:12 modulator accumulator      */

    uint32_t fb;           /* x0f feedback register      */
    uint32_t op1_out;      /* x10 operator-1 output      */
    uint32_t op2_out;      /* x11 operator-2 output      */

    uint32_t noise14, noise15;           /* x14 / x15 noise recurrence */
    uint32_t ring1_hist, ring1_delay;    /* x16 / y17 first resonator  */
    uint32_t ring2_hist, ring2_delay;    /* x18 / y19 output resonator */

    uint32_t block[32u];
    int have_block;
} mdkit_efm_sd;

extern const mdkit_voice mdkit_voice_efm_sd;

#endif /* MDKIT_EFM_SD_H */
