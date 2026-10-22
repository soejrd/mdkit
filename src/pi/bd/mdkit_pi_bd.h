/* SPDX-License-Identifier: CC0-1.0
 * PI-BD standalone synthesis voice.
 */
#ifndef MDKIT_PI_BD_H
#define MDKIT_PI_BD_H
#include <stdint.h>
#include "mdkit_voice.h"
typedef struct { uint32_t queued[8], last_raw[8]; unsigned targets, trigger, triggered; } mdkit_pi_bd_events;
typedef struct { uint32_t work[512], fraction[512], history[8][3]; } mdkit_pi_bd_modal;
typedef struct mdkit_pi_bd {
 mdkit_control_clock clock; mdkit_pi_bd_events events; unsigned pos;
 uint32_t pitch, decay, hardness, hammer, tension, damp;
 uint32_t phase_hi, phase_lo, attack, excitation_filter;
 uint32_t delay_pointer[3], delay_filter[3], feedback_hi, feedback_prev;
 mdkit_pi_bd_modal body; uint32_t delay[1536], block[32];
} mdkit_pi_bd;
enum { MDKIT_PI_BD_PTCH, MDKIT_PI_BD_DEC, MDKIT_PI_BD_HARD, MDKIT_PI_BD_HAMR, MDKIT_PI_BD_TENS, MDKIT_PI_BD_DAMP, MDKIT_PI_BD_CONTROLS };
extern const mdkit_voice mdkit_voice_pi_bd;
#endif
