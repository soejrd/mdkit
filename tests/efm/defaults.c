/* SPDX-License-Identifier: CC0-1.0 */
#include <stdio.h>
#include <stdlib.h>
#include "mdkit_voice.h"
#include "../../src/efm/bd/mdkit_efm_bd.h"
#include "../../src/efm/sd/mdkit_efm_sd.h"
#include "../../src/efm/xt/mdkit_efm_xt.h"
#include "../../src/efm/cp/mdkit_efm_cp.h"
#include "../../src/efm/rs/mdkit_efm_rs.h"
#include "../../src/efm/cb/mdkit_efm_cb.h"
#include "../../src/efm/hh/mdkit_efm_hh.h"
#include "../../src/efm/cy/mdkit_efm_cy.h"

int main(void)
{
    const mdkit_voice *voices[] = {&mdkit_voice_efm_bd,&mdkit_voice_efm_sd,&mdkit_voice_efm_xt,&mdkit_voice_efm_cp,&mdkit_voice_efm_rs,&mdkit_voice_efm_cb,&mdkit_voice_efm_hh,&mdkit_voice_efm_cy};
    const unsigned char expected[][8] = {
        {0, 64, 32, 127, 35, 14, 64, 24},
        {27, 60, 14, 79, 0, 5, 127, 29},
        {25, 50, 127, 111, 5, 4, 79, 0},
        {50, 40, 127, 24, 115, 36, 87, 8},
        {67, 10, 8, 15, 38, 25, 59, 0},
        {54, 61, 92, 56, 17, 112, 42},
        {26, 54, 26, 47, 0, 86, 93, 75},
        {64, 72, 96, 92, 45, 32, 64},
    };
    for (unsigned v = 0; v < sizeof voices / sizeof voices[0]; ++v) {
        const mdkit_voice *voice = voices[v];
        printf("Checking %s defaults...\n", voice->name); fflush(stdout);
        void *state = calloc(1, voice->state_bytes);
        if (!state || !voice->control_defaults) return 1;
        /* All EFM state declarations start with their control clock. */
        mdkit_control_clock *clock = state;
        for (unsigned pass = 0; pass < 2; ++pass) {
            voice->init(state);
            if (clock->count != voice->control_count) return 2;
            for (unsigned i = 0; i < voice->control_count; ++i) {
                const unsigned value = expected[v][i];
                if (voice->control_defaults[i] != value || clock->target[i] != value ||
                    clock->raw[i] != value || clock->current[i] != value || clock->pending[i] != value)
                    return 3;
                voice->set_control(state, i, 127u - value);
            }
        }
        free(state);
        printf("%s: published defaults and init/reset match specified controls\n", voice->name);
    }
    return 0;
}
