/* SPDX-License-Identifier: CC0-1.0
 * mdkit_efm_cp.h - EFM-CP standalone voice, machine 35.
 *
 * Caller-owned state and control indices. DSP behavior lives in the matching
 * .c file.
 */
#ifndef MDKIT_EFM_CP_H
#define MDKIT_EFM_CP_H
#include "mdkit_voice.h"
#include <stdint.h>

/* Stock knob order (md_web.c extra_controls for machine 35). */
enum {
    MDKIT_EFM_CP_PTCH = 0, MDKIT_EFM_CP_DEC, MDKIT_EFM_CP_CLPS, MDKIT_EFM_CP_CDEC,
    MDKIT_EFM_CP_MOD,      MDKIT_EFM_CP_MFRQ, MDKIT_EFM_CP_MDEC, MDKIT_EFM_CP_HPF,
    MDKIT_EFM_CP_CONTROLS
};

/* Slot roles, named from efm_cp_dsp.c:
 *   x1  PTCH  operator-1 rate            x5  MDEC  operator-2/feedback decay
 *   x2  DEC   operator-1 output decay    x6  0x7a1200 fixed modulator index
 *   x3  MFRQ  phase-1 rate scale         x8  fixed operator-1 feedback (0)
 *   x4  MOD   phase-2 modulation scale
 * Accumulators: L:c operator-1 phase, L:e operator-2 phase, L:13 modulator.
 * Registers: x0x10 operator-1 out, x0x11 operator-2 out.
 * Clipper: x0 initial = CDEC (y0xb), counter b = CLPS (y0x15), running
 * coefficient y0 = y0x12 (trigger 0x50c335), reload x0 = DEC (y2).
 * Tail: resonator y9 = HPF, history x0x17. */
typedef struct mdkit_efm_cp {
    mdkit_control_clock clock;
    unsigned pos;

    uint32_t op1_rate;     /* x1  PTCH */
    uint32_t op1_decay;    /* x2  DEC  */
    uint32_t mod_rate;     /* x3  MFRQ */
    uint32_t mod_amount;   /* x4  MOD  */
    uint32_t fb_decay;     /* x5  MDEC */
    uint32_t mod_index;    /* x6  fixed 0x7a1200 */
    uint32_t fb_coef;      /* x8  fixed 0 */
    uint32_t clipper_len;  /* CLPS */
    uint32_t clipper_decay;/* CDEC */
    uint32_t tail_coef;    /* HPF  */

    uint64_t l_c;          /* L:c  operator-1 phase accumulator */
    uint64_t l_e;          /* L:e  operator-2 phase accumulator */
    uint64_t l_13;         /* L:13 modulator accumulator      */

    uint32_t op1_out;      /* x0x10 operator-1 out */
    uint32_t op2_out;      /* x0x11 operator-2 out */

    uint32_t clip_b;       /* clipper counter (y0x15)   */
    uint32_t clip_env;     /* clipper running coeff (y0x12) */

    uint32_t res_hist, res_delay;   /* output resonator x0x17/... */

    uint32_t block[32u];
    int have_block;
} mdkit_efm_cp;

extern const mdkit_voice mdkit_voice_efm_cp;

#endif /* MDKIT_EFM_CP_H */
