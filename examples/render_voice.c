/* SPDX-License-Identifier: CC0-1.0
 * render_voice.c - minimal native renderer for one standalone voice.
 *
 * Usage: render_voice <output.raw> [samples] [control=value ...]
 * Writes interleaved mono float32 samples at 44.1 kHz.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include MDKIT_EXAMPLE_VOICE_HEADER

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <output.raw> [samples] [index=value ...]\n",
                argv[0]);
        return 2;
    }
    unsigned samples = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 44100u;
    const mdkit_voice *v = &VOICE_SYMBOL;
    void *state = calloc(1, v->state_bytes);
    float *block = malloc(257u * sizeof *block);
    if (!state || !block) { perror("allocation"); free(state); free(block); return 1; }

    v->init(state);
    for (int i = 3; i < argc; ++i) {
        char *eq = strchr(argv[i], '=');
        if (!eq) { fprintf(stderr, "bad control '%s'\n", argv[i]); return 2; }
        *eq = '\0';
        const unsigned index = (unsigned)strtoul(argv[i], NULL, 10);
        const unsigned value = (unsigned)strtoul(eq + 1, NULL, 10);
        if (index >= v->control_count) {
            fprintf(stderr, "control %u out of range (count %u)\n",
                    index, v->control_count);
            return 2;
        }
        v->set_control(state, index, value);
    }

    FILE *out = fopen(argv[1], "wb");
    if (!out) { perror("open"); free(state); free(block); return 1; }
    v->trigger(state);
    unsigned remaining = samples;
    while (remaining) {
        unsigned n = remaining > 257u ? 257u : remaining;
        v->render(state, block, n);
        if (fwrite(block, sizeof *block, n, out) != n) {
            perror("write"); fclose(out); free(state); free(block); return 1;
        }
        remaining -= n;
    }
    fclose(out);
    printf("%s: wrote %u samples to %s\n", v->name, samples, argv[1]);
    free(block);
    free(state);
    return 0;
}
