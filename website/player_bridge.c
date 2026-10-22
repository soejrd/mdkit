/* SPDX-License-Identifier: CC0-1.0
 * Player catalog, caller state and float output. No host imports or data files.
 */
#include "catalog.h"
#include <float.h>

#define WEB_CAPACITY 1024u
#define WEB_STATE_CAPACITY 1048576u
static union { double alignment; unsigned char bytes[WEB_STATE_CAPACITY]; } state;
static const mdkit_voice *voice;
static float output[WEB_CAPACITY];
static unsigned level = 127u;
static unsigned runtime_error;

unsigned mdkit_web_machine_count(void) { return (unsigned)MDKIT_CATALOG_COUNT; }
const char *mdkit_web_machine_name(unsigned kind)
{ return kind < MDKIT_CATALOG_COUNT ? mdkit_catalog[kind]->name : ""; }
unsigned mdkit_web_machine_id(unsigned kind)
{ return kind < MDKIT_CATALOG_COUNT ? mdkit_catalog[kind]->machine_id : 0u; }
unsigned mdkit_web_control_count(unsigned kind)
{ return kind < MDKIT_CATALOG_COUNT ? mdkit_catalog[kind]->control_count : 0u; }
const char *mdkit_web_control_name(unsigned kind, unsigned index)
{
    return kind < MDKIT_CATALOG_COUNT && index < mdkit_catalog[kind]->control_count
        ? mdkit_catalog[kind]->control_names[index] : "";
}
unsigned mdkit_web_default_control(unsigned kind, unsigned index)
{
    if (kind >= MDKIT_CATALOG_COUNT || index >= mdkit_catalog[kind]->control_count) return 0u;
    const unsigned char *defaults = mdkit_catalog[kind]->control_defaults;
    return defaults ? defaults[index] : 64u;
}
unsigned mdkit_web_init(unsigned kind)
{
    voice = 0;
    runtime_error = 0;
    if (kind >= MDKIT_CATALOG_COUNT || mdkit_catalog[kind]->state_bytes > sizeof state.bytes)
        return 0;
    /* Reset even fields a voice does not use in its initial phase. */
    for (unsigned i = 0; i < sizeof state.bytes; ++i) state.bytes[i] = 0;
    voice = mdkit_catalog[kind];
    voice->init(state.bytes);
    return 1;
}
void mdkit_web_set_control(unsigned index, unsigned value)
{
    if (voice && index < voice->control_count)
        voice->set_control(state.bytes, index, value > 127u ? 127u : value);
}
void mdkit_web_set_level(unsigned value) { level = value > 127u ? 127u : value; }
void mdkit_web_trigger(void) { if (voice) voice->trigger(state.bytes); }
float *mdkit_web_render(unsigned frames)
{
    if (frames > WEB_CAPACITY) frames = WEB_CAPACITY;
    if (voice) voice->render(state.bytes, output, frames);
    for (unsigned i = 0; i < frames; ++i) {
        float sample = voice ? output[i] : 0.0f;
        if (!(sample >= -FLT_MAX && sample <= FLT_MAX)) { runtime_error = 1; sample = 0.0f; }
        if (sample > 1.0f) sample = 1.0f;
        if (sample < -1.0f) sample = -1.0f;
        output[i] = sample * (0.5f * (float)level / 127.0f);
    }
    return output;
}
unsigned mdkit_web_capacity(void) { return WEB_CAPACITY; }
unsigned mdkit_web_native_sample_rate(void) { return 44100u; }
unsigned mdkit_web_runtime_error(void) { return runtime_error; }
