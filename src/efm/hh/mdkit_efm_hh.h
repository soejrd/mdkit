/* SPDX-License-Identifier: CC0-1.0
 * mdkit_efm_hh.h - EFM-HH standalone voice, machine 38.
 *
 * Caller-owned state and control indices. DSP behavior lives in the matching
 * .c file.
 */
#ifndef MDKIT_EFM_HH_H
#define MDKIT_EFM_HH_H
#include "mdkit_voice.h"
#include <stdint.h>

enum {
    MDKIT_EFM_HH_PTCH = 0, MDKIT_EFM_HH_DEC, MDKIT_EFM_HH_TREM, MDKIT_EFM_HH_TFRQ,
    MDKIT_EFM_HH_MOD,      MDKIT_EFM_HH_MFRQ, MDKIT_EFM_HH_MDEC, MDKIT_EFM_HH_FB,
    MDKIT_EFM_HH_CONTROLS
};

typedef struct mdkit_efm_hh {
    mdkit_control_clock clock;
    unsigned pos;
    unsigned have_block;
    /* per-block coefficient words (y1..y0xc) */
    uint32_t op_a_pitch;        /* y1  PTCH: op A pitch increment */
    uint32_t env_b_decay;       /* y2  DEC: op B envelope decay */
    uint32_t op_a_mod_freq;     /* y3  MFRQ: op A modulator frequency */
    uint32_t fm_index;          /* y4  MOD: FM index into op B */
    uint32_t env_a_amp;         /* y5  MDEC: op A envelope amplitude */
    uint32_t op_a_fb_index;     /* y6  0: op A self-PM index (unused) */
    uint32_t op_b_fb_index;     /* y7  FB: op B self-PM index */
    uint32_t tail_coef;         /* y8  0xf4240: resonance coefficient */
    uint32_t op_c_amp;          /* y9  TREM: op C amplitude */
    uint32_t op_c_offset;       /* y0xa 0x7fffff - TREM<<8 */
    uint32_t op_c_phase_inc;    /* y0xb TFRQ: op C phase increment */
    uint32_t op_c_scale;        /* y0xc 0xffffff: op C squaring scale */
    /* 48-bit phase accumulators */
    uint64_t phase_d;           /* L:d  op A phase */
    uint64_t phase_f;           /* L:f  op B phase */
    uint64_t phase_11;          /* L:11 op C phase */
    uint64_t mod_a;             /* L:16 op A PM offset (op_a_fb_index) */
    uint64_t mod_b;             /* L:18 op B PM offset (op_b_fb_index) */
    /* envelope registers */
    uint32_t env_a;             /* x:$14 op A envelope state */
    uint32_t env_b;             /* x:$15 op B envelope state */
    /* shared 2-pole resonance tail */
    uint32_t tail_hist;         /* x:$1a history */
    uint32_t tail_delay;        /* y:$1b previous output */
    uint32_t block[32];
} mdkit_efm_hh;

extern const mdkit_voice mdkit_voice_efm_hh;

#endif /* MDKIT_EFM_HH_H */
