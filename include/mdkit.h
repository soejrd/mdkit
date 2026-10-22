/* SPDX-License-Identifier: CC0-1.0
 * mdkit.h - standalone Machinedrum voice API.
 *
 * A voice is a caller-owned state block plus four operations. Controls are
 * 0..127, output is mono float at 44.1 kHz. One state block per instance.
 */
#ifndef MDKIT_H
#define MDKIT_H
#include <stddef.h>

#define MDKIT_MAX_CONTROLS 8u

typedef struct mdkit_voice {
    const char *name;
    unsigned machine_id;
    unsigned control_count;
    const char *const *control_names;
    size_t state_bytes;
    void (*init)(void *state);
    void (*set_control)(void *state, unsigned index, unsigned value);
    void (*trigger)(void *state);
    void (*render)(void *state, float *out, unsigned frames);
    /* control_count entries, in control_names order; NULL means midpoint (64). */
    const unsigned char *control_defaults;
} mdkit_voice;

#endif /* MDKIT_H */
