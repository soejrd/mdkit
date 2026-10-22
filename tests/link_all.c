/* SPDX-License-Identifier: CC0-1.0 */
#include <stdlib.h>
#include "catalog.h"
int main(void)
{
    float frame[64];
    for (unsigned i = 0; i < MDKIT_CATALOG_COUNT; ++i) {
        const mdkit_voice *v = mdkit_catalog[i];
        void *state = calloc(1, v->state_bytes);
        if (!state) return 1;
        v->init(state);
        v->trigger(state);
        v->render(state, frame, 64);
        for (unsigned j = 0; j < 64; ++j)
            if (!(frame[j] >= -1.0f && frame[j] <= 1.0f)) return 2;
        free(state);
    }
    return 0;
}
