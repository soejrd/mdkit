/* SPDX-License-Identifier: CC0-1.0
 * PI-SD standalone synthesis voice.
 */
#ifndef MDKIT_PI_SD_H
#define MDKIT_PI_SD_H
#include <stdint.h>
#include "mdkit_voice.h"
typedef struct { uint32_t queued[8], last_raw[8]; unsigned targets, trigger, triggered; } mdkit_pi_sd_events;
typedef struct { uint32_t work[512], fraction[512], history[8][3]; } mdkit_pi_sd_modal;
typedef struct mdkit_pi_sd {
 mdkit_control_clock clock; mdkit_pi_sd_events events; mdkit_pi_sd_modal body; unsigned pos, variant;
 uint32_t parameters[8], excitation_slow, excitation_fast, phase_hi, phase_lo, source_filter;
 uint32_t delay_position[3], delay_filter[3], delay_x[2][512], delay_y[512];
 uint32_t feedback, feedback_previous, noise_a, noise_b, rattle_threshold, rattle_gate, rattle_tension, noise_filter;
 uint32_t block[32];
} mdkit_pi_sd;
enum { MDKIT_PI_SD_PTCH, MDKIT_PI_SD_DEC, MDKIT_PI_SD_HARD, MDKIT_PI_SD_RING, MDKIT_PI_SD_TENS, MDKIT_PI_SD_RVOL, MDKIT_PI_SD_RDEC, MDKIT_PI_SD_CONTROLS };
extern const mdkit_voice mdkit_voice_pi_sd;
#endif
