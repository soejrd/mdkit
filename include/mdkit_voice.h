/* SPDX-License-Identifier: CC0-1.0
 * mdkit_voice.h - shared state declarations for standalone voices.
 *
 * DSP behavior lives in each voice's .c file. This header only declares the
 * per-instance control-clock layout that voice state headers may embed.
 */
#ifndef MDKIT_VOICE_H
#define MDKIT_VOICE_H
#include "mdkit.h"

typedef struct mdkit_control_clock {
    unsigned count;
    float    target[MDKIT_MAX_CONTROLS];
    float    raw[MDKIT_MAX_CONTROLS];
    float    current[MDKIT_MAX_CONTROLS];
    float    pending[MDKIT_MAX_CONTROLS];
    unsigned block;
    unsigned next_update;
    unsigned publish_block;
    int      pending_valid;
} mdkit_control_clock;

#endif /* MDKIT_VOICE_H */
