/* SPDX-License-Identifier: CC0-1.0
 * PI-ML standalone synthesis voice.
 */
#ifndef MDKIT_PI_ML_H
#define MDKIT_PI_ML_H
#include <stdint.h>
#include "mdkit_voice.h"
typedef struct { uint32_t queued[8], last_raw[8]; unsigned targets, trigger, triggered; } mdkit_pi_ml_events;
typedef struct { uint32_t work[512], fraction[512], history[8][3]; } mdkit_pi_ml_modal;
typedef struct mdkit_pi_ml {
 mdkit_control_clock clock; mdkit_pi_ml_events events; unsigned pos;
 uint32_t pitch, decay, hardness, mode_spacing, tension, tension_decay, mode_gain;
 uint32_t allpass_gain, mode_decay, amplitude, phase_lo, phase_hi, excitation_filter, attack;
 uint32_t feedback_hi, feedback_prev, noise_a, noise_b, ring_a, ring_b, ring_c;
 mdkit_pi_ml_modal body; uint32_t block[32];
} mdkit_pi_ml;
enum { MDKIT_PI_ML_PTCH, MDKIT_PI_ML_DEC, MDKIT_PI_ML_HARD, MDKIT_PI_ML_TENS, MDKIT_PI_ML_CONTROLS };
extern const mdkit_voice mdkit_voice_pi_ml;
#endif
