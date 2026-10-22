/* SPDX-License-Identifier: CC0-1.0
 * PI-MA standalone synthesis voice.
 */
#ifndef MDKIT_PI_MA_H
#define MDKIT_PI_MA_H
#include <stdint.h>
#include "mdkit_voice.h"
typedef struct { uint32_t queued[8], last_raw[8]; unsigned targets, trigger, triggered; } mdkit_pi_ma_events;
typedef struct { uint32_t work[512], fraction[512], history[8][3]; } mdkit_pi_ma_modal;
typedef struct mdkit_pi_ma {
 mdkit_control_clock clock; mdkit_pi_ma_events events;
 uint32_t grain_threshold, output_gain, decay, grain_memory, feedback_a, feedback_b;
 uint32_t envelope, grain_level, filter_a, filter_b, previous, noise_a, noise_b;
 uint32_t block[32]; unsigned pos;
} mdkit_pi_ma;
enum { MDKIT_PI_MA_GRNS, MDKIT_PI_MA_DEC, MDKIT_PI_MA_GLEN, MDKIT_PI_MA_SIZE, MDKIT_PI_MA_HARD, MDKIT_PI_MA_CONTROLS };
extern const mdkit_voice mdkit_voice_pi_ma;
#endif
