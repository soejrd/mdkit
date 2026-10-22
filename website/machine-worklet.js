/* SPDX-License-Identifier: CC0-1.0 */
/* global AudioWorkletProcessor, registerProcessor, sampleRate */
class MachineProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.wasm = null;
    this.samples = null;
    this.kind = 0;
    this.controls = Array(8).fill(0);
    this.level = 127;
    this.tempo = 120;
    this.steps = Array(16).fill(false);
    this.step = 0;
    this.untilStep = 0;
    this.playing = false;
    this.trigger = false;
    this.port.onmessage = event => this.message(event.data);
  }
  async message(message) {
    try {
      if (message.type === 'load') {
        this.wasm = (await WebAssembly.instantiate(message.bytes, {})).instance.exports;
        if (this.wasm.mdkit_web_native_sample_rate() !== sampleRate)
          throw new Error('Voice requires a 44.1 kHz audio context.');
        const count = this.wasm.mdkit_web_machine_count();
        const defaults = Array.from({length: count}, (_, kind) =>
          Array.from({length: 8}, (_, index) => this.wasm.mdkit_web_default_control(kind, index)));
        const decode = (ptr, kind, index) => {
          if (!ptr) return '';
          const buf = new Uint8Array(this.wasm.memory.buffer);
          let s = '';
          for (let i = ptr; i < buf.length && buf[i] !== 0; i++) s += String.fromCharCode(buf[i]);
          return s;
        };
        const info = Array.from({length: count}, (_, kind) => {
          const n = this.wasm.mdkit_web_control_count(kind);
          const labels = Array.from({length: n}, (_, index) =>
            decode(this.wasm.mdkit_web_control_name(kind, index), kind, index));
          return {name: decode(this.wasm.mdkit_web_machine_name(kind), kind), labels};
        });
        if (!this.wasm.mdkit_web_init(0)) throw new Error('Could not initialize the voice.');
        this.samples = new Float32Array(this.wasm.memory.buffer,
          this.wasm.mdkit_web_render(0), this.wasm.mdkit_web_capacity());
        this.port.postMessage({type: 'ready', defaults, info});
      } else if (message.type === 'machine') {
        this.kind = message.kind;
        this.controls = [...message.controls];
        this.trigger = false;
        if (this.wasm) {
          if (!this.wasm.mdkit_web_init(this.kind)) throw new Error('Could not initialize the voice.');
          this.controls.forEach((value, index) => this.wasm.mdkit_web_set_control(index, value));
          this.wasm.mdkit_web_set_level(this.level);
        }
      } else if (message.type === 'control') {
        this.controls[message.index] = message.value;
        this.wasm?.mdkit_web_set_control(message.index, message.value);
      } else if (message.type === 'level') {
        this.level = message.value;
        this.wasm?.mdkit_web_set_level(message.value);
      } else if (message.type === 'tempo') {
        const tempo = Number(message.value);
        if (!Number.isFinite(tempo)) return;
        const next = Math.max(30, Math.min(240, tempo));
        // Keep the current position within the step when changing speed.
        if (this.playing) this.untilStep *= this.tempo / next;
        this.tempo = next;
      } else if (message.type === 'step') {
        this.steps[message.index] = message.active;
      } else if (message.type === 'trigger') {
        this.trigger = true;
      } else if (message.type === 'play') {
        this.step = 0;
        this.untilStep = 0;
        this.playing = true;
      } else if (message.type === 'stop') {
        this.playing = false;
      }
    } catch (error) {
      if (message.type === 'load') { this.wasm = null; this.samples = null; }
      this.port.postMessage({type: 'error', message: String(error.message || error)});
    }
  }
  process(_inputs, outputs) {
    const channels = outputs[0];
    if (!channels?.[0] || !this.wasm || !this.samples) return true;
    const left = channels[0], right = channels[1];
    let offset = 0;
    while (offset < left.length) {
      if (this.playing && this.untilStep <= 0) {
        this.port.postMessage({type: 'playhead', index: this.step});
        if (this.steps[this.step]) this.trigger = true;
        this.step = (this.step + 1) % 16;
        this.untilStep += sampleRate * 60 / (this.tempo * 4);
      }
      if (this.trigger) {
        this.wasm.mdkit_web_trigger();
        this.trigger = false;
      }
      const untilStep = this.playing ? Math.max(1, Math.ceil(this.untilStep)) : left.length;
      const frames = Math.min(this.samples.length, left.length - offset, untilStep);
      this.wasm.mdkit_web_render(frames);
      if(this.wasm.mdkit_web_runtime_error?.()) {
        this.port.postMessage({type: 'error', message: 'Voice rendering failed; reload the player.'});
        this.wasm=null; this.samples=null; return true;
      }
      for (let i = 0; i < frames; i++) {
        const value = Math.max(-1, Math.min(1, this.samples[i]));
        left[offset + i] = value;
        if (right) right[offset + i] = value;
      }
      offset += frames;
      if (this.playing) this.untilStep -= frames;
    }
    return true;
  }
}
registerProcessor('machine-processor', MachineProcessor);
