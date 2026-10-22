#!/usr/bin/env node
/* SPDX-License-Identifier: CC0-1.0
 * check_wasm.cjs - verify one single-voice WASM module and render a short signal.
 *
 * Usage: node tests/check_wasm.cjs <voice.wasm> [prefix]
 */
'use strict';

const fs = require('fs');

async function main() {
  const file = process.argv[2];
  const prefix = process.argv[3];
  if (!file) {
    console.error('usage: node tests/check_wasm.cjs <voice.wasm> [prefix]');
    process.exit(2);
  }

  const bytes = fs.readFileSync(file);
  const module = await WebAssembly.compile(bytes);
  const imports = WebAssembly.Module.imports(module);
  if (imports.length) {
    console.error(`FAIL unexpected imports: ${imports.map(i => `${i.module}.${i.name}`).join(', ')}`);
    process.exit(1);
  }

  const instance = await WebAssembly.instantiate(module, {});
  const wasm = instance.exports;
  const guessed = Object.keys(wasm).find(k => k.endsWith('_sample_rate'));
  const p = prefix || (guessed ? guessed.slice(0, -'_sample_rate'.length) : '');
  if (!p) {
    console.error('FAIL cannot determine export prefix');
    process.exit(1);
  }

  const required = ['state_size', 'init', 'set_control', 'trigger', 'render', 'sample_rate'];
  for (const suffix of required) {
    if (typeof wasm[`${p}_${suffix}`] !== 'function') {
      console.error(`FAIL missing export ${p}_${suffix}`);
      process.exit(1);
    }
  }

  if (wasm[`${p}_sample_rate`]() !== 44100) {
    console.error('FAIL sample rate is not 44100');
    process.exit(1);
  }
  if (wasm[`${p}_state_size`]() === 0) {
    console.error('FAIL empty voice state');
    process.exit(1);
  }

  wasm[`${p}_init`]();
  wasm[`${p}_trigger`]();
  let peak = 0;
  let nonfinite = 0;
  for (let block = 0; block < 64; ++block) {
    const ptr = wasm[`${p}_render`](256);
    const samples = new Float32Array(wasm.memory.buffer, ptr, 256);
    for (const sample of samples) {
      if (!Number.isFinite(sample)) ++nonfinite;
      peak = Math.max(peak, Math.abs(sample));
    }
  }
  if (nonfinite) {
    console.error(`FAIL ${nonfinite} non-finite samples`);
    process.exit(1);
  }
  if (peak === 0) {
    console.error('FAIL silence after trigger');
    process.exit(1);
  }
  if (peak > 1.000001) {
    console.error(`FAIL peak ${peak} exceeds [-1,1]`);
    process.exit(1);
  }
  console.log(`${p}: WASM exports, no imports, 44.1 kHz, peak ${peak.toFixed(4)} OK`);
}

main().catch(error => {
  console.error(error);
  process.exit(1);
});
