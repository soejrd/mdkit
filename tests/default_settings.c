/* SPDX-License-Identifier: CC0-1.0 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "catalog.h"
#include "mdkit_voice.h"
static const struct { const char *name; unsigned count; unsigned char knobs[8]; } expected[] = {
    {"EFM-BD", 8, {0, 64, 32, 127, 35, 14, 64, 24}},
    {"EFM-SD", 8, {27, 60, 14, 79, 0, 5, 127, 29}},
    {"EFM-XT", 8, {25, 50, 127, 111, 5, 4, 79, 0}},
    {"EFM-CP", 8, {50, 40, 127, 24, 115, 36, 87, 8}},
    {"EFM-RS", 8, {67, 10, 8, 15, 38, 25, 59, 0}},
    {"EFM-CB", 7, {54, 61, 92, 56, 17, 112, 42}},
    {"EFM-HH", 8, {26, 54, 26, 47, 0, 86, 93, 75}},
    {"EFM-CY", 7, {64, 72, 96, 92, 45, 32, 64}},
    {"TRX-B2", 8, {60, 65, 41, 62, 91, 44, 56, 58}},
    {"TRX-BD", 8, {64, 64, 0, 0, 0, 0, 0, 0}},
    {"TRX-SD", 8, {35, 7, 0, 64, 127, 0, 100, 90}},
    {"TRX-XT", 7, {70, 25, 50, 30, 64, 32, 0}},
    {"TRX-CP", 8, {50, 100, 7, 127, 40, 64, 0, 90}},
    {"TRX-RS", 3, {50, 0, 50}},
    {"TRX-CB", 6, {100, 50, 127, 127, 0, 0}},
    {"TRX-CH", 5, {0, 8, 0, 127, 0}},
    {"TRX-OH", 5, {50, 60, 8, 127, 0}},
    {"TRX-CY", 6, {127, 0, 0, 127, 0, 127}},
    {"TRX-XC", 7, {64, 32, 32, 100, 0, 0, 0}},
    {"TRX-CL", 6, {64, 32, 64, 0, 64, 0}},
    {"TRX-MA", 8, {32, 32, 0, 0, 64, 64, 64, 64}},
    {"PI-BD", 6, {64, 35, 100, 0, 127, 100}},
    {"PI-SD", 7, {64, 64, 10, 40, 90, 80, 80}},
    {"PI-MT", 8, {20, 20, 127, 127, 127, 80, 90, 64}},
    {"PI-ML", 4, {0, 64, 127, 0}},
    {"PI-MA", 5, {90, 0, 72, 96, 64}},
    {"PI-RS", 6, {90, 48, 64, 64, 64, 64}},
    {"PI-RC", 8, {64, 70, 96, 32, 127, 127, 64, 127}},
    {"PI-CC", 8, {64, 70, 90, 32, 127, 127, 64, 127}},
    {"PI-HH", 8, {64, 70, 100, 30, 127, 127, 60, 127}},
};
int main(void)
{
    if (MDKIT_CATALOG_COUNT > sizeof expected / sizeof expected[0]) return 1;
    for (unsigned k = 0; k < MDKIT_CATALOG_COUNT; ++k) {
        const mdkit_voice *v = mdkit_catalog[k];
        unsigned e = 0;
        while (e < sizeof expected / sizeof expected[0] && strcmp(expected[e].name,v->name)) ++e;
        if (e == sizeof expected / sizeof expected[0] || expected[e].count != v->control_count || !v->control_defaults) return 2;
        void *state = calloc(1,v->state_bytes);
        if (!state) return 3;
        mdkit_control_clock *clock = state;
        for (unsigned pass = 0; pass < 2; ++pass) {
            v->init(state);
            for (unsigned i = 0; i < v->control_count; ++i) {
                unsigned value = expected[e].knobs[i];
                if (v->control_defaults[i] != value || clock->target[i] != value ||
                    clock->raw[i] != value || clock->current[i] != value || clock->pending[i] != value) {
                    printf("FAIL %s default %u\n",v->name,i); return 4;
                }
                v->set_control(state,i,127-value);
            }
        }
        free(state);
        printf("%s: descriptor, init and reset match specified defaults\n",v->name);
    }
    return 0;
}
