/* SPDX-License-Identifier: CC0-1.0
 * Render a fresh note without interrupting live playback. */
async function renderSample({kind, controls, level}) {
  const fetchBytes = async path => {
    const response = await fetch(path, {cache: 'no-store'});
    if (!response.ok) throw new Error(`Could not load ${path} (${response.status}).`);
    return response.arrayBuffer();
  };
  const bytes = await fetchBytes('mdkit.wasm');
  const wasm = (await WebAssembly.instantiate(bytes, {})).instance.exports;
  if (wasm.mdkit_web_native_sample_rate() !== 44100) throw new Error('Sample export requires a 44.1 kHz engine.');
  if (!wasm.mdkit_web_init(kind)) throw new Error('Voice unavailable. Rebuild the player.');
  controls.forEach((value, index) => wasm.mdkit_web_set_control(index, value));
  wasm.mdkit_web_set_level(level);
  const samples = new Float32Array(wasm.memory.buffer, wasm.mdkit_web_render(0), wasm.mdkit_web_capacity());
  const pcm = new Int32Array(44100 * 10);
  wasm.mdkit_web_trigger();
  let peak = 0;
  for (let offset = 0; offset < pcm.length;) {
    const frames = Math.min(samples.length, pcm.length - offset);
    wasm.mdkit_web_render(frames);
    if (wasm.mdkit_web_runtime_error?.()) throw new Error('Voice rendering failed during sample export.');
    for (let i = 0; i < frames; i++) {
      const sample = Math.max(-8388608, Math.min(8388607, Math.round(samples[i] * 8388608)));
      pcm[offset + i] = sample;
      peak = Math.max(peak, Math.abs(sample));
    }
    offset += frames;
  }
  // Fixed-point voices can retain tiny DC offsets or noise indefinitely. Treat
  // tails below -90 dBFS as silence, capped at -60 dB relative to the hit's peak
  // so deliberately quiet sounds keep their decay. Do not normalize the audio.
  const silenceThreshold = Math.max(1, Math.min(8388608 * Math.pow(10, -90 / 20), peak * .001));
  let lastSound = pcm.length;
  while (lastSound > 0 && Math.abs(pcm[lastSound - 1]) <= silenceThreshold) lastSound--;
  // Keep the attack, at least 250 ms, and 100 ms after the last audible sample.
  // Long/continuous voices are still bounded to ten seconds.
  const frames = Math.min(pcm.length, Math.max(11025, lastSound + 4410));
  if (frames < pcm.length) {
    // Fade only the final 5 ms of padding, removing residual DC at the cut.
    const fadeFrames = 221;
    for (let i = 0; i < fadeFrames; i++) {
      pcm[frames - fadeFrames + i] = Math.round(pcm[frames - fadeFrames + i] * (1 - i / (fadeFrames - 1)));
    }
  }
  const wav = new ArrayBuffer(44 + frames * 3);
  const header = new DataView(wav);
  const text = (offset, value) => [...value].forEach((char, i) => header.setUint8(offset + i, char.charCodeAt(0)));
  text(0, 'RIFF'); header.setUint32(4, wav.byteLength - 8, true); text(8, 'WAVE');
  text(12, 'fmt '); header.setUint32(16, 16, true); header.setUint16(20, 1, true);
  header.setUint16(22, 1, true); header.setUint32(24, 44100, true);
  header.setUint32(28, 44100 * 3, true); header.setUint16(32, 3, true); header.setUint16(34, 24, true);
  text(36, 'data'); header.setUint32(40, frames * 3, true);
  const output = new Uint8Array(wav);
  for (let i = 0; i < frames; i++) {
    output[44 + i * 3] = pcm[i] & 255;
    output[45 + i * 3] = (pcm[i] >> 8) & 255;
    output[46 + i * 3] = (pcm[i] >> 16) & 255;
  }
  // RIFF chunks must end on an even byte boundary (padding is not sample data).
  if (wav.byteLength % 2) {
    const padded = new Uint8Array(wav.byteLength + 1);
    padded.set(output);
    new DataView(padded.buffer).setUint32(4, padded.byteLength - 8, true);
    return padded.buffer;
  }
  return wav;
}
self.onmessage = async ({data}) => {
  try {
    const wav = await renderSample(data);
    self.postMessage({wav}, [wav]);
  } catch (error) { self.postMessage({error: error.message}); }
};
