# Standalone TRX technical checks

Run from the release folder:

```sh
python3 tests/trx/family/run.py --voices bd,xt,cl,cb,ch,oh,cy,cp,ma,xc,b2
```

The runner accepts `--source-root` for a candidate source tree containing `<voice>/mdkit_trx_<voice>.{c,h}`, and `--out` for build evidence. It reads only the chosen C source/header files and public release support files. The selected voices supply all synthesis behavior. `CC`, `WASM_CC`, and `NODE` may override tools. `--no-wasm` runs only native checks.

The per-voice harness uses the same descriptor/header preprocessor parameters as the root harness. It checks silence before trigger; audible finite output bounded to [-1,1]; exact equivalence between contiguous rendering and sizes1,7,31,257,32,63,511; independence of interleaved instances; and a retrigger that has an observable signal effect. A retrigger need not change an arbitrary percentage of samples.

First-control endpoint tests queue0 or127, let the control clock settle for44100 samples, and then trigger. They check finite, bounded audible output and compare the entire29.7second endpoint streams. This duration captures hold/gap controls whose first effect occurs after the initial attack. A difference exceeding one24-bit output LSB establishes an observable response; changed-sample counts are printed as diagnostics only.

Every remaining control is also settled and exercised at0/127, with finite/bounded output required (silence can be valid for gain endpoints). Default-setting tails are checked and reported through10seconds.

Envelope RMS/peak measurements are printed at approximately.37,1.1,5.2,10.0,29.7seconds for both endpoint settings. These expose decay, quantized tails, and long holds without imposing one universal decay speed. Finite bounded tails are required; sound/envelope acceptance remains an owner listening decision.

A deterministic control/trigger schedule produces16384 native samples. A no-import WASM module executes the identical event schedule and irregular sizes. The runner reports maximum and RMS differences, accepting at most1e-5 peak numerical deviation across native/WASM. Exact equality currently holds for all tested TRX candidates. Finally, every selected native object links in one process to detect external symbol collisions and verify distinct valid descriptors.

These checks establish technical behavior, not sound fidelity or listening approval.
