/* SPDX-License-Identifier: CC0-1.0
 * PI-HH standalone synthesis voice.
 */
#ifndef MDKIT_PI_HH_H
#define MDKIT_PI_HH_H
#include <stdint.h>
#include "mdkit_voice.h"
typedef struct { uint32_t queued[8], last_raw[8]; unsigned targets, trigger, triggered; } mdkit_pi_hh_events;
typedef struct { uint32_t work[512], fraction[512], history[8][3]; } mdkit_pi_hh_modal;
typedef struct { uint32_t unused; uint32_t pitch; uint32_t decay; uint32_t hardness; uint32_t ring; uint32_t window_step; uint32_t coupling_gain; uint32_t coupling_angle; uint32_t brightness; uint32_t grab; uint32_t reserved_0a; uint32_t reserved_0b; uint32_t reserved_0c; uint32_t reserved_0d; uint32_t reserved_0e; uint32_t reserved_0f; uint32_t impulse_step; uint32_t excitation_decay; uint32_t excitation_threshold; uint32_t window_budget; uint32_t page_a; uint32_t page_b; uint32_t page_c; uint32_t page_d; uint32_t reserved_18; uint32_t reserved_19; uint32_t reserved_1a; uint32_t reserved_1b; uint32_t delay_history_a; uint32_t delay_history_b; uint32_t delay_history_c; uint32_t delay_history_d; uint32_t reserved_20; uint32_t first_fraction; uint32_t first_history; uint32_t first_previous; uint32_t first_input; uint32_t first_input_previous; uint32_t second_fraction; uint32_t second_history; uint32_t second_previous; uint32_t second_input; uint32_t second_input_previous; uint32_t output_fraction; uint32_t output_history; uint32_t output_previous; uint32_t output_input; uint32_t output_input_previous; uint32_t noise_a; uint32_t noise_b; uint32_t amplitude; uint32_t attack_gate; uint32_t excitation_envelope; uint32_t excitation_cursor; uint32_t excitation_count; uint32_t gate; uint32_t attack_step; uint32_t ring_amplitude; uint32_t close_memory; uint32_t close_envelope; uint32_t close_decay; uint32_t reserved_3d; uint32_t reserved_3e; uint32_t reserved_3f; } mdkit_pi_hh_state_words;
typedef union { mdkit_pi_hh_state_words voice; uint32_t state_words[64]; } mdkit_pi_hh_storage;
typedef struct mdkit_pi_hh {
 mdkit_control_clock clock; mdkit_pi_hh_events events; unsigned pos, machine;
 mdkit_pi_hh_storage storage; uint32_t work[1024], fraction[1024], delay[65536], block[32];
} mdkit_pi_hh;
enum { MDKIT_PI_HH_PTCH, MDKIT_PI_HH_DEC, MDKIT_PI_HH_CLSN, MDKIT_PI_HH_RING, MDKIT_PI_HH_AG, MDKIT_PI_HH_AU, MDKIT_PI_HH_BR, MDKIT_PI_HH_CLOS, MDKIT_PI_HH_CONTROLS };
extern const mdkit_voice mdkit_voice_pi_hh;
#endif
