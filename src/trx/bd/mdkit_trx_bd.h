/* SPDX-License-Identifier: CC0-1.0
 * TRX-BD standalone synthesis voice.
 */
#ifndef MDKIT_TRX_BD_H
#define MDKIT_TRX_BD_H
#include <stdint.h>
#include "mdkit_voice.h"
typedef struct { uint32_t queued[8],last_raw[8];unsigned targets,trigger,triggered;} mdkit_trx_bd_events;






enum {
    MDKIT_TRX_BD_PTCH = 0, MDKIT_TRX_BD_DEC, MDKIT_TRX_BD_RAMP, MDKIT_TRX_BD_RDEC,
    MDKIT_TRX_BD_STRT,     MDKIT_TRX_BD_NOIS, MDKIT_TRX_BD_HARM, MDKIT_TRX_BD_CLIP,
    MDKIT_TRX_BD_CONTROLS
};


#define MDKIT_TRX_BD_TRIG_CLIP_THRESH 0x7ffcb9u
#define MDKIT_TRX_BD_TRIG_SLEW        0x7fdbb4u
#define MDKIT_TRX_BD_TRIG_NOIS_STEP   0x000032u

typedef struct mdkit_trx_bd {
    mdkit_control_clock clock;
    mdkit_trx_bd_events events;
    unsigned pos;


    uint32_t dec;
    uint32_t strt;
    uint32_t ptch;
    uint32_t ramp;
    uint32_t rdec;
    uint32_t clip[5];
    uint32_t nois;
    uint32_t spread;
    uint32_t harm;
    uint32_t slew;
    uint32_t xfade;
    uint32_t exc;
    uint32_t decay_acc;
    uint32_t env;
    uint32_t clip_th;
    uint32_t fold_lo;
    uint32_t fold_hi;
    uint32_t phase_a;
    uint32_t phase_b;
    uint32_t tail_amp;
    uint32_t board_ofs;
    uint32_t fold_ref;
    uint32_t fold_ref2;
    uint32_t kv_a;
    uint32_t kv_b;
    uint32_t kv_c;
    uint32_t ph2_a;
    uint32_t ph2_b;
    uint32_t ph2_step;

    uint32_t y[0x60];
    uint32_t x[0x60];
    uint32_t out[0x20];
    uint32_t r7;
    uint32_t block[32];
    unsigned have_block;
} mdkit_trx_bd;

extern const mdkit_voice mdkit_voice_trx_bd;


#endif
