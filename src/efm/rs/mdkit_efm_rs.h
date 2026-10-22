/* SPDX-License-Identifier: CC0-1.0
 * mdkit_efm_rs.h - EFM-RS standalone voice, machine 36.
 *
 * Caller-owned state and control indices. DSP behavior lives in the matching
 * .c file.
 */
#ifndef MDKIT_EFM_RS_H
#define MDKIT_EFM_RS_H
#include "mdkit_voice.h"
#include <stdint.h>

/* Stock knob order (md_web.c extra_controls for machine 36). */
enum {
    MDKIT_EFM_RS_PTCH = 0, MDKIT_EFM_RS_DEC, MDKIT_EFM_RS_MOD, MDKIT_EFM_RS_HPF,
    MDKIT_EFM_RS_SNAR,     MDKIT_EFM_RS_SPTC, MDKIT_EFM_RS_SDEC, MDKIT_EFM_RS_SMOD,
    MDKIT_EFM_RS_CONTROLS
};

/* Slot roles, named from efm_rs_dsp.c (X index = s->y index):
 *   group A (modulated 2-op):  x1  SPTC base rate   x2  0x600000 fixed
 *                              x3  SMOD modulator    x4  0x7a1200 fixed index
 *                              x9  SDEC A-output decay
 *   group B (modulated 2-op):  x5  PTCH base rate   x6  0x600000 fixed
 *                              x7  MOD modulator      x8  0x7a1200 fixed index
 *                              x0a DEC  B-output decay
 *                              x0b SNAR snare mix gain         x0c HPF tail coef
 * Accumulators: ld A phase, l17 A modulator, lf A second-stage phase,
 * l11 B phase, l19 B modulator, l13 B second-stage phase.
 * Registers: x0x15 A output, x0x16 B output, x0x1d A stage (snare input).
 * Tail: x0x1b resonator history, y0x1c previous output word. */
typedef struct mdkit_efm_rs {
    mdkit_control_clock clock;
    unsigned pos;

    uint32_t a_rate, a_const, a_mod, a_index;   /* x1 x2 x3 x4  */
    uint32_t b_rate, b_const, b_mod, b_index;   /* x5 x6 x7 x8  */
    uint32_t a_out_decay, b_out_decay;          /* x9 x0a       */
    uint32_t snare_gain, tail_coef;             /* x0b x0c      */

    uint64_t a_phase, a_modacc, a_phase2;       /* ld l17 lf    */
    uint64_t b_phase, b_modacc, b_phase2;       /* l11 l19 l13  */

    uint32_t a_out, b_out, a_stage;             /* x0x15 x0x16 x0x1d */

    uint32_t res_hist, res_delay;               /* x0x1b / y0x1c */

    uint32_t block[32u];
    int have_block;
} mdkit_efm_rs;

extern const mdkit_voice mdkit_voice_efm_rs;

#endif /* MDKIT_EFM_RS_H */
