/* SPDX-License-Identifier: CC0-1.0
 * wasm_bridge.c - single-voice WASM ABI. No libc or host imports required.
 */
#include <stddef.h>
#include MDKIT_EXAMPLE_VOICE_HEADER

#define JOIN2_(a, b) a##b
#define JOIN2(a, b) JOIN2_(a, b)
#define EXPORT(name) JOIN2(VOICE_PREFIX, name)

static VOICE_PREFIX voice_state;
static float render_buffer[1024];

unsigned EXPORT(_state_size)(void) { return (unsigned)VOICE_SYMBOL.state_bytes; }

void EXPORT(_init)(void) { VOICE_SYMBOL.init(&voice_state); }

void EXPORT(_set_control)(unsigned index, unsigned value)
{ VOICE_SYMBOL.set_control(&voice_state, index, value); }

void EXPORT(_trigger)(void) { VOICE_SYMBOL.trigger(&voice_state); }

float *EXPORT(_render)(unsigned frames)
{
    if (frames > 1024u) frames = 1024u;
    VOICE_SYMBOL.render(&voice_state, render_buffer, frames);
    return render_buffer;
}

unsigned EXPORT(_sample_rate)(void) { return 44100u; }
