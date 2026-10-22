/* SPDX-License-Identifier: CC0-1.0
 * Exercise the actual worklet, live tempo and export worker against release WASM.
 */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const root = path.resolve(__dirname, '../..');
const read = name => fs.readFileSync(path.join(root, 'website', name));
const buffer = bytes => bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
async function make() {
  let Processor;
  vm.runInNewContext(read('machine-worklet.js').toString(), {
    sampleRate: 44100, WebAssembly, Math, Float32Array, Uint8Array,
    AudioWorkletProcessor: class {
      constructor() { this.messages = []; this.port = {postMessage: message => this.messages.push(message)}; }
    },
    registerProcessor: (_, value) => { Processor = value; }
  });
  const p = new Processor();
  await p.message({type: 'load', bytes: read('mdkit.wasm')});
  assert.equal(p.messages.at(-1).type, 'ready', JSON.stringify(p.messages));
  return p;
}
(async () => {
  const module = await WebAssembly.compile(read('mdkit.wasm'));
  assert.equal(WebAssembly.Module.imports(module).length, 0, 'module has no imports');
  const p = await make();
  const info = p.messages.find(message => message.type === 'ready');
  const registrations = fs.readdirSync(path.join(root, 'src'), {withFileTypes: true})
    .filter(e => e.isDirectory()).flatMap(family => fs.readdirSync(path.join(root, 'src', family.name))
      .filter(voice => fs.existsSync(path.join(root, 'src', family.name, voice, 'voice.mk'))));
  assert.equal(info.info.length, registrations.length, 'catalog contains every registered voice');
  assert.equal(new Set(info.info.map(v => v.name)).size, registrations.length, 'unique catalog names');
  assert.equal(new Set(info.info.map((_, i) => p.wasm.mdkit_web_machine_id(i))).size, registrations.length, 'unique machine IDs');
  assert.equal(info.info[0].name, 'TRX-B2', 'startup instrument is TRX-B2');
  const families = [...new Set(info.info.map(voice => voice.name.split('-')[0]))];
  assert.deepEqual(Array.from(families), ['TRX', 'EFM', 'PI'].filter(family => families.includes(family)),
    'selector families follow TRX, EFM, PI order');
  const expectedDefaults = {"EFM-BD": [0, 64, 32, 127, 35, 14, 64, 24], "EFM-SD": [27, 60, 14, 79, 0, 5, 127, 29], "EFM-XT": [25, 50, 127, 111, 5, 4, 79, 0], "EFM-CP": [50, 40, 127, 24, 115, 36, 87, 8], "EFM-RS": [67, 10, 8, 15, 38, 25, 59, 0], "EFM-CB": [54, 61, 92, 56, 17, 112, 42], "EFM-HH": [26, 54, 26, 47, 0, 86, 93, 75], "EFM-CY": [64, 72, 96, 92, 45, 32, 64], "TRX-B2": [60, 65, 41, 62, 91, 44, 56, 58], "TRX-BD": [64, 64, 0, 0, 0, 0, 0, 0], "TRX-SD": [35, 7, 0, 64, 127, 0, 100, 90], "TRX-XT": [70, 25, 50, 30, 64, 32, 0], "TRX-CP": [50, 100, 7, 127, 40, 64, 0, 90], "TRX-RS": [50, 0, 50], "TRX-CB": [100, 50, 127, 127, 0, 0], "TRX-CH": [0, 8, 0, 127, 0], "TRX-OH": [50, 60, 8, 127, 0], "TRX-CY": [127, 0, 0, 127, 0, 127], "TRX-XC": [64, 32, 32, 100, 0, 0, 0], "TRX-CL": [64, 32, 64, 0, 64, 0], "TRX-MA": [32, 32, 0, 0, 64, 64, 64, 64], "PI-BD": [64, 35, 100, 0, 127, 100], "PI-SD": [64, 64, 10, 40, 90, 80, 80], "PI-MT": [20, 20, 127, 127, 127, 80, 90, 64], "PI-ML": [0, 64, 127, 0], "PI-MA": [90, 0, 72, 96, 64], "PI-RS": [90, 48, 64, 64, 64, 64], "PI-RC": [64, 70, 96, 32, 127, 127, 64, 127], "PI-CC": [64, 70, 90, 32, 127, 127, 64, 127], "PI-HH": [64, 70, 100, 30, 127, 127, 60, 127]};
  assert(info.info.every(voice => Object.hasOwn(expectedDefaults, voice.name)), "every catalog voice has specified defaults");
  for (const [name, values] of Object.entries(expectedDefaults)) {
    const kind = info.info.findIndex(voice => voice.name === name);
    if (kind < 0) continue; // Only registered voices belong to this build.
    assert.deepEqual(Array.from(info.defaults[kind]).slice(0, values.length), values,
      `${name} browser defaults match the requested knob order`);
    assert.equal(info.info[kind].labels.length, values.length);
    for (let i = values.length; i < 8; i++) assert.equal(info.defaults[kind][i], 0);
  }
  // Measure real playhead boundaries through the worklet's render path.
  const audio = new Float32Array(128), right = new Float32Array(128);
  const timing = await make();
  const tick = frames => timing.process([], [[new Float32Array(frames)]]);
  const heads = () => timing.messages.filter(m => m.type === 'playhead');
  await timing.message({type: 'play'});
  tick(1000);
  assert.equal(heads().length, 1);
  await timing.message({type: 'tempo', value: 240});
  tick(2257);
  assert.equal(heads().length, 1, 'tempo edit preserves the current step');
  tick(1);
  assert.equal(heads().length, 2, '120 to 240 BPM speeds up the current step immediately');
  assert.equal(heads().at(-1).index, 1, 'tempo change does not restart the pattern');
  await timing.message({type: 'tempo', value: 60});
  tick(11018);
  assert.equal(heads().length, 2);
  tick(1);
  assert.equal(heads().length, 3, '240 to 60 BPM slows down the current step immediately');
  await timing.message({type: 'stop'});
  tick(20000);
  assert.equal(heads().length, 3, 'stop halts sequencing');
  await timing.message({type: 'play'});
  tick(1);
  assert.equal(heads().at(-1).index, 0, 'play restarts at step one');
  await p.message({type: 'machine', kind: 0, controls: Array(8).fill(64)});
  await p.message({type: 'step', index: 0, active: true});
  await p.message({type: 'play'});
  p.process([], [[audio, right]]);
  assert.deepEqual(audio, right, 'mono duplicated to both channels');
  await p.message({type: 'control', index: 0, value: 80});
  assert.equal(p.controls[0], 80, 'knobs remain live during sequencing');
  await p.message({type: 'stop'});
  await p.message({type: 'step', index: 0, active: false});
  await p.message({type: 'step', index: 0, active: true});
  assert.equal(p.steps[0], true);
  // The public interface and processor have no per-step parameter editing.
  for (const file of ['script.js', 'machine-worklet.js', 'index.html']) {
    assert(!/P-lock|selectedStep|appliedLocks|type: ['"]locks['"]|id=['"]lock['"]/.test(read(file).toString()));
  }
  const scope = {WebAssembly, Math, Float32Array, Int32Array, Uint8Array, ArrayBuffer, DataView,
    fetch: async file => {
      assert.equal(file, 'mdkit.wasm', 'export fetches only the release module');
      return {ok: true, arrayBuffer: async () => buffer(read(file))};
    }, self: {}};
  vm.createContext(scope);
  vm.runInContext(read('sample-worker.js').toString(), scope);
  for (let kind = 0; kind < info.info.length; kind++) {
    const name = info.info[kind].name;
    const controls = Array.from(info.defaults[kind]);
    assert.equal(controls.length, 8);
    assert(info.info[kind].labels.length >= 1 && info.info[kind].labels.length <= 8);
    await p.message({type: 'machine', kind, controls});
    p.process([], [[audio]]);
    assert(audio.every(value => value === 0), `${name} silent before trigger`);
    // WAV is a separate fresh instance, with exactly the live mix gain.
    const wav = await scope.renderSample({kind, controls, level: 93});
    const view = new DataView(wav);
    assert.equal(Buffer.from(wav).toString('ascii', 0, 4), 'RIFF');
    assert.equal(view.getUint32(4, true), wav.byteLength - 8);
    assert.equal(view.getUint32(24, true), 44100);
    assert.equal(view.getUint16(22, true), 1);
    assert.equal(view.getUint16(34, true), 24);
    assert.equal(wav.byteLength % 2, 0);
    const frames = view.getUint32(40, true) / 3;
    assert(frames >= 11025 && frames <= 441000);
    await p.message({type: 'machine', kind, controls});
    await p.message({type: 'level', value: 93});
    await p.message({type: 'trigger'});
    let nonzero = false;
    for (let offset = 0; offset < 8192; offset += 128) {
      p.process([], [[audio]]);
      for (let i = 0; i < 128; i++) {
        const at = 44 + (offset + i) * 3;
        const sample = (view.getUint8(at) | view.getUint8(at + 1) << 8 | view.getUint8(at + 2) << 16) << 8 >> 8;
        assert(Number.isFinite(audio[i]) && Math.abs(audio[i]) <= .5);
        assert(Math.abs(sample / 8388608 - audio[i]) <= 1 / 8388608, `${name} export/live sample ${offset + i}`);
        nonzero ||= sample !== 0;
      }
    }
    assert(nonzero, `${name} audible`);
    console.log(`${name}: catalog, silence, playback, 24-bit export/live parity OK (${frames} frames)`);
  }
  assert(!p.messages.some(m => m.type === 'error'), JSON.stringify(p.messages));
  const bad = await make();
  await bad.message({type: 'machine', kind: 999, controls: []});
  assert.equal(bad.messages.at(-1).type, 'error', 'missing voice never falls back');
  console.log(`${info.info.length} voices: no imports, live tempo, sequencing, reset and export checks passed`);
})().catch(error => { console.error(error); process.exit(1); });
