/* SPDX-License-Identifier: CC0-1.0
 * mdkit_efm_cb.h - EFM-CB standalone voice, machine 37.
 *
 * Caller-owned state and control indices. DSP behavior lives in the matching
 * .c file.
 */
#ifndef MDKIT_EFM_CB_H
#define MDKIT_EFM_CB_H
#include "mdkit_voice.h"
#include <stdint.h>

enum {
    MDKIT_EFM_CB_PTCH = 0, MDKIT_EFM_CB_DEC, MDKIT_EFM_CB_SNAP, MDKIT_EFM_CB_FB,
    MDKIT_EFM_CB_MOD,      MDKIT_EFM_CB_MFRQ, MDKIT_EFM_CB_MDEC,
    MDKIT_EFM_CB_CONTROLS
};

typedef struct mdkit_efm_cb {
    mdkit_control_clock clock;
    unsigned pos;
    unsigned have_block;
    /* per-block coefficient words (y1..y0xa) */
    uint32_t pitch_a;      /* y1  op A pitch */
    uint32_t pitch_c;      /* y2  op C pitch (pitch*379>>8) */
    uint32_t mod_freq;     /* y3  MFRQ: modulator frequency of op A/C */
    uint32_t fm_index;     /* y4  MOD: FM index into op B/D */
    uint32_t mdec;         /* y5  MDEC: decay of the feedback modulator env */
    uint32_t fb_index;     /* y6  FB: feedback index into op B/D modulators */
    uint32_t env1_amp;     /* y7  0x7ffe00 - SNAP*420 */
    uint32_t dec;          /* y8  DEC: env1 decay */
    uint32_t env2_amp;     /* y9  SNAP*420 */
    uint32_t env2_decay;   /* y0xa DEC_TAIL: env2 decay */
    /* 48-bit phase accumulators */
    uint64_t phase_b;      /* L:b  op A phase */
    uint64_t phase_d;      /* L:d  op B phase */
    uint64_t phase_f;      /* L:f  op C phase */
    uint64_t phase_11;     /* L:11 op D phase */
    /* 48-bit modulator offsets */
    uint64_t mod_d;        /* L:16 self-PM of op B (fb_index * sine D) */
    uint64_t mod_11;       /* L:18 self-PM of op D (fb_index * sine B) */
    /* feedback/env registers */
    uint32_t fb;           /* x:$13 feedback register (PM into op C) */
    uint32_t env1;         /* x:$14 first decaying envelope state */
    uint32_t env2;         /* x:$15 second decaying envelope state */
    uint32_t block[32];
} mdkit_efm_cb;

extern const mdkit_voice mdkit_voice_efm_cb;

#endif /* MDKIT_EFM_CB_H */
