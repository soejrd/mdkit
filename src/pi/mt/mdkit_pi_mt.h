/* SPDX-License-Identifier: CC0-1.0
 * PI-MT standalone synthesis voice.
 */
#ifndef MDKIT_PI_MT_H
#define MDKIT_PI_MT_H
#include <stdint.h>
#include "mdkit_voice.h"
typedef struct { uint32_t queued[8], last_raw[8]; unsigned targets, trigger, triggered; } mdkit_pi_mt_events;
typedef struct { uint32_t work[512], fraction[512], history[8][3]; } mdkit_pi_mt_modal;
typedef struct mdkit_pi_mt {
 mdkit_control_clock clock; mdkit_pi_mt_events events; mdkit_pi_mt_modal body; unsigned pos, variant;
 uint32_t parameters[8], excitation_slow, excitation_fast, phase_hi, phase_lo, source_filter;
 uint32_t delay_position[3], delay_filter[3], delay_x[2][512], delay_y[512];
 uint32_t feedback, feedback_previous, noise_a, noise_b, rattle_threshold, rattle_gate, rattle_tension, noise_filter;
 uint32_t block[32];
} mdkit_pi_mt;
enum { MDKIT_PI_MT_PTCH, MDKIT_PI_MT_DEC, MDKIT_PI_MT_HARD, MDKIT_PI_MT_HAMR, MDKIT_PI_MT_TUNE, MDKIT_PI_MT_DAMP, MDKIT_PI_MT_SIZE, MDKIT_PI_MT_POS, MDKIT_PI_MT_CONTROLS };
extern const mdkit_voice mdkit_voice_pi_mt;
#endif
