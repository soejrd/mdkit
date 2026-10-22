/* SPDX-License-Identifier: CC0-1.0
 * mdkit_efm_cy.h - EFM-CY standalone voice, machine 39.
 *
 * Caller-owned state and control indices. DSP behavior lives in the matching
 * .c file.
 */
#ifndef MDKIT_EFM_CY_H
#define MDKIT_EFM_CY_H
#include "mdkit_voice.h"
#include <stdint.h>

/* Stock knob order (md_web.c extra_controls for machine 39). */
enum {
    MDKIT_EFM_CY_PTCH = 0, MDKIT_EFM_CY_DEC, MDKIT_EFM_CY_FB, MDKIT_EFM_CY_HPF,
    MDKIT_EFM_CY_MOD,      MDKIT_EFM_CY_MFRQ, MDKIT_EFM_CY_MDEC,
    MDKIT_EFM_CY_CONTROLS
};

/* Slot roles, named from efm_cy_dsp.c:
 *   x1..x4  PM base rates for operators 0..3 (spectral partials, PTCH-derived)
 *   x5      amplitude envelope decay (DEC)
 *   x6..x9  circular-scan pointer increments  (MFRQ-derived factor)
 *   x0xa    wave amplitude        (MOD)
 *   x0xb    wave envelope decay     (MDEC)
 *   x0xc    FM feedback coefficient (FB)
 *   x0xd    HP resonator coefficient (HPF)
 *   x0xe    FM feedback envelope  (FB/derived)
 *   x0x0f/x0x11/x0x13/x0x15 circular scan pointers
 *   x0x10/x0x12/x0x14/x0x16 FM phase words
 *   x0x17 wave envelope   x0x18 amplitude envelope
 *   x0x19..x0x1c FM feedback registers
 *   x0x1d/x0x1e first HP history/previous, x0x1f/x0x20 second HP pair. */
typedef struct mdkit_efm_cy {
    mdkit_control_clock clock;
    unsigned pos;

    uint32_t fm_rate[4];      /* x1..x4  PTCH-derived partial rates */
    uint32_t env_decay;       /* x5      DEC envelope decay        */
    uint32_t scan_step[4];    /* x6..x9  MFRQ-derived pointer step  */
    uint32_t wave_amp;        /* x0xa    MOD wave amplitude        */
    uint32_t wave_env_decay;  /* x0xb    DEC envelope decay        */
    uint32_t fm_fb;           /* x0xc    FB feedback coefficient   */
    uint32_t hp_coef;         /* x0xd    HPF resonator coefficient */
    uint32_t fb_env_decay;    /* x0xe    fixed 0x400000 gain scale  */

    uint32_t scan_ptr[4];     /* x0x0f/x0x11/x0x13/x0x15 */
    uint32_t fm_phase[4];     /* x0x10/x0x12/x0x14/x0x16 */
    uint32_t wave_env;        /* x0x17 */
    uint32_t amp_env;         /* x0x18 */
    uint32_t fm_fb_reg[4];    /* x0x19..x0x1c */

    uint32_t hp1_hist, hp1_delay;   /* x0x1d / x0x1e */
    uint32_t hp2_hist, hp2_delay;   /* x0x1f / x0x20 */

    uint32_t block[32u];
    int have_block;
} mdkit_efm_cy;

extern const mdkit_voice mdkit_voice_efm_cy;

#endif /* MDKIT_EFM_CY_H */
