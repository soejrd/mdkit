/* SPDX-License-Identifier: CC0-1.0
 * test_voice.c - host-side contract and behavior checks for a standalone voice.
 *
 * Usage: test_voice <voice.h> <voice_symbol> <machine_id>
 * The generated wrapper under tests/build/ supplies the three arguments.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include MDKIT_TEST_VOICE_HEADER

static int failures;

#define CHECK(cond, message) do { \
    if (!(cond)) { printf("FAIL %s\n", (message)); ++failures; } \
} while (0)

static double peak(const float *x, unsigned n)
{
    double p = 0.0;
    for (unsigned i = 0; i < n; ++i) {
        const double a = fabs((double)x[i]);
        if (a > p) p = a;
    }
    return p;
}

static double rms(const float *x, unsigned n)
{
    double a = 0.0;
    for (unsigned i = 0; i < n; ++i) a += (double)x[i] * (double)x[i];
    return sqrt(a / (double)n);
}

static int finite_bounded(const float *x, unsigned n)
{
    for (unsigned i = 0; i < n; ++i) {
        if (!isfinite((double)x[i])) return 0;
        if (x[i] < -1.0f || x[i] > 1.0f) return 0;
    }
    return 1;
}

int main(void)
{
    const mdkit_voice *v = &VOICE_SYMBOL;
    CHECK(v->machine_id == VOICE_MACHINE_ID, "machine id");
    CHECK(v->control_count > 0 && v->control_count <= MDKIT_MAX_CONTROLS,
          "control count");
    CHECK(v->state_bytes > 0, "state size");
    CHECK(v->name != NULL, "name");

    void *state = calloc(1, v->state_bytes);
    float *a = malloc(44100u * sizeof *a);
    float *b = malloc(44100u * sizeof *b);
    if (!state || !a || !b) { printf("FAIL allocation\n"); return 1; }

    v->init(state);
    v->render(state, a, 512);
    CHECK(peak(a, 512) == 0.0, "silent before trigger");

    v->init(state);
    v->trigger(state);
    v->render(state, a, 44100);
    CHECK(peak(a, 44100) > 0.01, "audible after trigger");
    CHECK(finite_bounded(a, 44100), "finite and bounded");
    CHECK(rms(a, 2000) > 10.0 * rms(a + 40000, 4000), "decays over note");

    v->init(state);
    v->trigger(state);
    v->render(state, a, 3000);
    v->init(state);
    v->trigger(state);
    unsigned pos = 0;
    while (pos < 3000) {
        unsigned n = 3000u - pos;
        if (n > 7u) n = 7u;
        v->render(state, b + pos, n);
        pos += n;
    }
    CHECK(memcmp(a, b, 3000u * sizeof *a) == 0,
          "render length does not change output");

    void *sibling = calloc(1, v->state_bytes);
    if (!sibling) { printf("FAIL allocation\n"); return 1; }
    v->init(state);
    v->init(sibling);
    v->trigger(state);
    v->render(state, a, 1000);
    v->render(sibling, b, 1000);
    CHECK(peak(b, 1000) == 0.0, "instance isolation");

    v->init(state);
    v->set_control(state, 0, 0);
    v->trigger(state);
    v->render(state, a, 4000);
    v->init(state);
    v->set_control(state, 0, 127);
    v->trigger(state);
    v->render(state, b, 4000);
    /* Short default notes can finish well before this window ends. Measure
     * signal change rather than requiring a minimum number of sounding frames. */
    double difference_energy = 0.0;
    for (unsigned i = 0; i < 4000; ++i) {
        const double delta = (double)a[i] - (double)b[i];
        difference_energy += delta * delta;
    }
    CHECK(difference_energy > 1e-4, "first control extremes differ");

    v->init(state);
    v->trigger(state);
    v->render(state, a, 2000);
    v->trigger(state);
    v->render(state, a + 2000, 2000);
    CHECK(peak(a + 2000, 2000) > 0.001, "retrigger remains audible");
    CHECK(finite_bounded(a + 2000, 2000), "retrigger remains bounded");

    free(sibling);
    free(b);
    free(a);
    free(state);
    if (failures) { printf("%d checks failed\n", failures); return 1; }
    printf("%s: ABI, silence, decay, sizes, isolation, extremes, retrigger OK\n",
           v->name);
    return 0;
}
