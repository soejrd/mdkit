# mdkit

mdkit brings the EFM, TRX and PI families into standalone C99.

[Try them yourself →](https://soejrd.github.io/mdkit/)

Build them into an instrument or port them to your favourite language.

## Voices

| **Family** | **Voices** |
| ---------- | ---------- |
| EFM | BD, SD, XT, CP, RS, CB, HH, CY |
| TRX | BD, SD, XT, CP, RS, CB, CH, OH, CY, MA, CL, XC, B2 |
| PI | BD, SD, MT, ML, MA, RS, RC, CC, HH |

## Audio properties

| **Property** | **Implementation** |
| ------------ | ------------------ |
| Language | C99 |
| Output | Mono float32 audio |
| Core sample rate | 44,100 Hz |
| State | Caller-owned, separate for each instance |
| Internal arithmetic | Voice-specific; fixed-point behavior retained where required |
| Controls | 0–127, with per-voice defaults |

## C API

To use a voice in C, compile its `.c` file, include its `.h`, and add `include/`
to your header search path. You can use individual voices without building the
whole collection or the showcase player.

Every voice exposes the same four operations: `init`, `set_control`, `trigger`
and `render`. Here is one short hit:

```c
#include "mdkit_efm_xt.h"

void render_hit(float audio[128])
{
    mdkit_efm_xt state;
    const mdkit_voice *voice = &mdkit_voice_efm_xt;

    voice->init(&state);
    voice->set_control(&state, MDKIT_EFM_XT_DEC, 96);
    voice->trigger(&state);
    voice->render(&state, audio, 128);
}
```

For continuous playback, initialize once and keep the same state between render
calls. Rendering accepts arbitrary frame counts. Give each instance its own
state and avoid accessing it concurrently. Initialization also resets it to its
default controls. Applications choose their own output gain.

When translating a voice, preserve rounding, saturation, wrap behavior and event
timing: they help shape the sound. Controls may be smoothed or applied at a
trigger rather than on the very next sample. Check retriggers, extreme settings,
long tails and changes across render-buffer boundaries against the C version.
Changing the core sample rate requires adapting and checking the synthesis.

## Build and check

With a C99 compiler and Make:

```sh
make native
./out/efm_xt/render_voice output.raw 44100 0=64 4=96
```

The example writes one second of raw mono float32 audio. See
[examples/render_voice.c](examples/render_voice.c) for a complete usage example.
The checks in `tests/` provide behavior references for ports.

`make all` builds and tests native voices, WASM modules and the showcase player;
`make ready` also checks the complete package. These full checks require Python
3.9+, Node.js, and Clang with the wasm32 backend and wasm-ld. The build detects
Homebrew LLVM on macOS; otherwise set `WASM_CC` to your compatible Clang.

## License

mdkit is available under **CC0 1.0 Universal**. See [LICENSE](LICENSE) and
[NOTICE](NOTICE) for the dedication and its scope.

mdkit is an independent project, unaffiliated with or endorsed by Elektron.
Machinedrum and Elektron names identify the instrument and its manufacturer.
