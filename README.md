# mdkit
Portable C drum synthesis voices based on the Elektron Machinedrum.

mdkit is being developed as a collection of individual synthesis voices for
developers building instruments, audio plugins and embedded audio applications.
The goal is to make each voice usable on its own, with a small shared support
layer and a straightforward C interface.

---

All machinedrum machines will soon be available on a digitakt too, I'm resynthesizing them all and porting to digitakt. 

---

## Voices

The planned scope is 30 voices across three families:

| Family | Voices |
| --- | --- |
| EFM | BD, SD, XT, CP, RS, CB, HH, CY |
| TRX | BD, SD, XT, CP, RS, CB, CH, OH, CY, MA, CL, XC, B2 |
| PI | BD, SD, MT, ML, MA, RS, RC, CC, HH |

## Audio and numerical behavior

| Property | First-release target |
| --- | --- |
| Language | C99 |
| Output | Mono float32 audio |
| Core sample rate | 44,100 Hz |
| Render length | Any positive number of frames |
| State | Caller-owned, separate for each instance |
| Internal arithmetic | Voice-specific; fixed-point behavior retained where required |

## Porting

The intended delivery is source code that can be compiled with only the selected
voices and their required helpers. Porting documentation will cover memory
alignment, dependencies, output gain, event ordering and numerical assumptions.

Preserve rounding, saturation, wrap behavior and event timing when optimizing a
core. Check triggers and parameter changes across buffer boundaries, as well as
long tails and extreme settings. CPU and memory requirements depend on the voice,
compiler and target processor.

Platform adapters and native sample-rate variants can be developed separately.
Compatibility with any particular hardware instrument depends on that platform's
extension facilities and a tested integration.

## Fidelity

The aim is to preserve the character and behavior of the voices across their
control ranges. 

## Project and licensing

mdkit is an independent project, unaffiliated with or endorsed by Elektron.
Machinedrum and Elektron names identify the instrument and its manufacturer.

The source release will include its license and any required third-party notices.
No license for the forthcoming implementation is granted by this draft README.
