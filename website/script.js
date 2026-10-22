/* SPDX-License-Identifier: CC0-1.0 */
let machines = [];
const machineSelect = document.getElementById('machine');
const machinePicker = document.getElementById('machine-picker');
const machineMenu = document.getElementById('machine-menu');
const familyContainer = document.getElementById('machine-families');
const voiceContainer = document.getElementById('machine-voices');
const chevron = machineSelect.querySelector('svg').cloneNode(true);
let menuFamily = null;
const controlContainer = document.getElementById('controls');
const stepContainer = document.getElementById('steps');
const status = document.getElementById('status');
const defaults = machines.map(() => Array(8).fill(0));
const steps = Array(16).fill(false);
let kind = 0, controls = [], context = null, node = null, ready = null;
let infoRequest = null;
let playing = false, analyser = null;
let instrumentsReady = null, instrumentBytes = null;
function refreshStep(index) {
  const cell = stepContainer.children[index];
  const button = cell.querySelector('button');
  button.classList.toggle('active', steps[index]);
  button.setAttribute('aria-pressed', String(steps[index]));
  cell.querySelector('.step-led').classList.toggle('lit', steps[index]);
}

function setStatus(message, error = false) {
  status.textContent = message;
  status.classList.toggle('error', error);
}
function send(message) { node?.port.postMessage(message); }
function updateDisplay() {
  document.getElementById('machine-title').textContent = machines[kind].name;
  document.getElementById('machine-choice').textContent = machines[kind].name;
  document.getElementById('machine-number').textContent =
    `MACHINE ${String(kind + 1).padStart(2, '0')} / ${machines.length}`;
}
function drawControls() {
  controlContainer.replaceChildren();
  updateDisplay();
  machines[kind].labels.forEach((name, index) => {
    if (name === '—') return;
    const control = document.createElement('div');
    control.className = 'knob-control';
    control.innerHTML = `<div class="knob-scale"><svg viewBox="0 0 100 100" aria-hidden="true"><path d="M19 80a43 43 0 1 1 62 0" fill="none" stroke="currentColor" stroke-width="1.5" stroke-dasharray="1 7.8"/></svg><div class="knob" role="slider" tabindex="0" aria-valuemin="0" aria-valuemax="127"><div class="knob-face"><span></span></div></div></div><div class="knob-name"></div><output class="knob-value"></output>`;
    const knob = control.querySelector('.knob');
    const value = control.querySelector('output');
    control.querySelector('.knob-name').textContent = name;
    knob.setAttribute('aria-label', name);
    knob.setAttribute('aria-orientation', 'vertical');
    const update = next => {
      const current = Math.max(0, Math.min(127, Math.round(next)));
      value.textContent = String(current);
      knob.setAttribute('aria-valuenow', String(current));
      knob.querySelector('.knob-face').style.transform = `rotate(${-135 + current / 127 * 270}deg)`;
      return current;
    };
    const change = next => {
      const current = Math.max(0, Math.min(127, Math.round(next)));
      controls[index] = current;
      send({type: 'control', index, value: current});
      update(current);
    };
    update(controls[index]);
    knob.addEventListener('pointerdown', event => {
      if (event.button !== 0) return;
      event.preventDefault();
      knob.focus();
      knob.setPointerCapture(event.pointerId);
      const start = event.clientY, initial = controls[index];
      const move = e => change(initial + (start - e.clientY) * (e.shiftKey ? .1 : .6));
      const end = () => {
        knob.removeEventListener('pointermove', move);
        knob.removeEventListener('lostpointercapture', end);
      };
      knob.addEventListener('pointermove', move);
      knob.addEventListener('lostpointercapture', end);
    });
    knob.addEventListener('keydown', event => {
      const delta = {ArrowUp: 1, ArrowRight: 1, ArrowDown: -1, ArrowLeft: -1, PageUp: 10, PageDown: -10}[event.key];
      if (delta || event.key === 'Home' || event.key === 'End') {
        event.preventDefault();
        change(event.key === 'Home' ? 0 : event.key === 'End' ? 127 : controls[index] + delta);
      }
    });
    knob.addEventListener('wheel', event => {
      event.preventDefault();
      if (event.deltaY) change(controls[index] + (event.deltaY < 0 ? 1 : -1));
    }, {passive: false});
    controlContainer.append(control);
  });
}
function setPlaying(value) {
  playing = value;
  document.getElementById('play').classList.toggle('running', playing);
  document.getElementById('play').setAttribute('aria-pressed', String(playing));
  document.getElementById('play-label').textContent = playing ? 'Pause' : 'Play';
  if (!playing) [...stepContainer.children].forEach(step => step.classList.remove('current'));
}
function drawScope() {
  const data = new Float32Array(1024);
  const scope = document.getElementById('scope');
  const draw = () => {
    if (!analyser) return;
    if (!document.hidden) {
      analyser.getFloatTimeDomainData(data);
      scope.setAttribute('d', Array.from({length: 461}, (_, i) =>
        `${i === 0 ? 'M' : 'L'}${i} ${(52 - Math.max(-1, Math.min(1, data[i * 2])) * 48).toFixed(2)}`).join(''));
    }
    requestAnimationFrame(draw);
  };
  draw();
}
async function loadInstruments() {
  if (instrumentsReady) return instrumentsReady;
  instrumentsReady = (async () => {
    const response = await fetch('mdkit.wasm', {cache: 'no-store'});
    if (!response.ok) throw new Error(`Could not load mdkit.wasm (${response.status}). Run make web.`);
    instrumentBytes = await response.arrayBuffer();
    const wasm = (await WebAssembly.instantiate(instrumentBytes, {})).instance.exports;
    const strings = new Uint8Array(wasm.memory.buffer);
    const decode = ptr => {
      let text = '';
      while (ptr && strings[ptr]) text += String.fromCharCode(strings[ptr++]);
      return text;
    };
    infoRequest = Array.from({length: wasm.mdkit_web_machine_count()}, (_, voice) => ({
      name: decode(wasm.mdkit_web_machine_name(voice)),
      labels: Array.from({length: wasm.mdkit_web_control_count(voice)}, (_, index) =>
        decode(wasm.mdkit_web_control_name(voice, index)))
    }));
    infoRequest.forEach((_, voice) => {
      defaults[voice] = Array.from({length: 8}, (_, index) => wasm.mdkit_web_default_control(voice, index));
    });
    machines = infoRequest;
    kind = 0;
    controls = [...defaults[kind]];
    buildMachineMenu();
    drawControls();
    machineSelect.disabled = false;
    document.getElementById('download').disabled = false;
    setStatus(`Ready. ${machines.length} instruments. Play a sound or start your pattern.`);
  })().catch(error => {
    instrumentsReady = null;
    instrumentBytes = null;
    setStatus(error.message, true);
    throw error;
  });
  return instrumentsReady;
}

async function ensureAudio() {
  if (ready) return ready;
  ready = (async () => {
    if (!window.AudioWorkletNode) throw new Error('AudioWorklet is unavailable in this browser.');
    context = new AudioContext({sampleRate: 44100});
    if (context.sampleRate !== 44100) throw new Error(`A 44.1 kHz audio context is required; got ${context.sampleRate} Hz.`);
    await context.audioWorklet.addModule('machine-worklet.js');
    await loadInstruments();
    const bytes = instrumentBytes.slice(0);
    node = new AudioWorkletNode(context, 'machine-processor', {
      numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2]
    });
    analyser = context.createAnalyser();
    analyser.fftSize = 1024;
    node.connect(analyser);
    analyser.connect(context.destination);
    const loaded = new Promise((resolve, reject) => {
      node.port.onmessage = event => {
        const message = event.data;
        if (message.type === 'ready') resolve();
        else if (message.type === 'error') {
          setStatus(message.message, true);
          setPlaying(false);
          reject(new Error(message.message));
        }
        else if (message.type === 'playhead') {
          [...stepContainer.children].forEach((step, i) =>
            step.classList.toggle('current', playing && i === message.index));
        }
      };
    });
    node.port.postMessage({type: 'load', bytes}, [bytes]);
    await loaded;
    send({type: 'machine', kind, controls});
    send({type: 'level', value: 127});
    send({type: 'tempo', value: Number(document.getElementById('tempo').value)});
    steps.forEach((active, index) => send({type: 'step', index, active}));
    drawScope();
    setStatus(`Ready. ${machines.length} EFM/TRX/PI instruments.`);
  })().catch(async error => {
    node?.disconnect();
    analyser?.disconnect();
    analyser = null;
    if (context && context.state !== 'closed') await context.close();
    context = null; node = null; ready = null;
    setStatus(error.message, true);
    throw error;
  });
  return ready;
}

function chooseMachine(index) {
  kind = index;
  controls = [...defaults[kind]];
  drawControls();
  send({type: 'machine', kind, controls});
  const latched = ['TRX-CP','TRX-MA','TRX-CL'].includes(infoRequest[kind]?.name);
  setStatus(latched ? 'Parameter changes take effect on the next trigger.' : 'Ready.');
  closeMachineMenu(true);
}
function closeMachineMenu(focus = false) {
  machineMenu.hidden = true;
  machineSelect.setAttribute('aria-expanded', 'false');
  if (focus) machineSelect.focus();
}
function showFamily(family, focus = false) {
  menuFamily = family;
  for (const button of familyContainer.children) {
    button.setAttribute('aria-expanded', String(button.dataset.family === family));
  }
  voiceContainer.replaceChildren();
  voiceContainer.setAttribute('aria-label', `${family} voices`);
  machines.forEach((machine, index) => {
    if (infoRequest[index].name.split('-')[0] !== family) return;
    const button = document.createElement('button');
    button.type = 'button';
    button.setAttribute('role', 'menuitemradio');
    button.setAttribute('aria-checked', String(index === kind));
    button.classList.toggle('chosen', index === kind);
    button.textContent = machine.name;
    button.addEventListener('click', () => chooseMachine(index));
    voiceContainer.append(button);
  });
  if (focus) voiceContainer.querySelector('button')?.focus();
}
function buildMachineMenu() {
  familyContainer.replaceChildren();
  const families = [...new Set(infoRequest.map(machine => machine.name.split('-')[0]))];
  families.forEach(family => {
    const button = document.createElement('button');
    button.type = 'button';
    button.dataset.family = family;
    button.setAttribute('role', 'menuitem');
    button.setAttribute('aria-haspopup', 'menu');
    button.setAttribute('aria-controls', 'machine-voices');
    button.setAttribute('aria-expanded', 'false');
    button.append(family, chevron.cloneNode(true));
    button.addEventListener('click', () => showFamily(family, true));
    button.addEventListener('pointerenter', event => {
      if (event.pointerType === 'mouse' && !familyContainer.contains(document.activeElement) &&
          !voiceContainer.contains(document.activeElement)) showFamily(family);
    });
    familyContainer.append(button);
  });
}
machineSelect.addEventListener('click', () => {
  if (!machineMenu.hidden) { closeMachineMenu(); return; }
  showFamily(infoRequest[kind].name.split('-')[0]);
  machineMenu.hidden = false;
  machineSelect.setAttribute('aria-expanded', 'true');
});
machineSelect.addEventListener('keydown', event => {
  if (!['ArrowDown', 'ArrowUp'].includes(event.key)) return;
  event.preventDefault();
  if (machineMenu.hidden) machineSelect.click();
  const buttons = [...familyContainer.children];
  buttons[event.key === 'ArrowDown' ? 0 : buttons.length - 1]?.focus();
});
machineMenu.addEventListener('keydown', event => {
  const inFamilies = familyContainer.contains(event.target);
  const container = inFamilies ? familyContainer : voiceContainer;
  const buttons = [...container.querySelectorAll('button')];
  const index = buttons.indexOf(document.activeElement);
  if (['ArrowDown', 'ArrowUp', 'Home', 'End'].includes(event.key)) {
    event.preventDefault();
    const next = event.key === 'Home' ? 0 : event.key === 'End' ? buttons.length - 1
      : (index + (event.key === 'ArrowDown' ? 1 : -1) + buttons.length) % buttons.length;
    buttons[next]?.focus();
  } else if (event.key === 'ArrowRight' && inFamilies) {
    event.preventDefault();
    showFamily(event.target.dataset.family, true);
  } else if (event.key === 'ArrowLeft' && !inFamilies) {
    event.preventDefault();
    [...familyContainer.children].find(button => button.dataset.family === menuFamily)?.focus();
  } else if (event.key === 'Escape') {
    event.preventDefault();
    event.stopPropagation();
    closeMachineMenu(true);
  }
});
document.addEventListener('pointerdown', event => {
  if (!machinePicker.contains(event.target)) closeMachineMenu();
});
machinePicker.addEventListener('focusout', event => {
  if (!machinePicker.contains(event.relatedTarget)) closeMachineMenu();
});
for (let index = 0; index < 16; index++) {
  const cell = document.createElement('div');
  cell.className = `step-cell ${index % 4 === 0 ? 'beat-start' : ''}`;
  const led = document.createElement('div');
  led.className = 'step-led';
  const button = document.createElement('button');
  button.className = 'step'; button.type = 'button';
  button.textContent = String(index + 1).padStart(2, '0');
  button.setAttribute('aria-label', `Step ${index + 1}`);
  button.setAttribute('aria-pressed', 'false');
  button.addEventListener('click', () => {
    steps[index] = !steps[index];
    refreshStep(index);
    send({type: 'step', index, active: steps[index]});
  });
  const beat = document.createElement('span');
  beat.className = 'beat-label';
  beat.textContent = index % 4 === 0 ? `0${index / 4 + 1}` : '·';
  cell.append(led, button, beat);
  stepContainer.append(cell);
}
const tempo = document.getElementById('tempo');
function setTempo(value) {
  const next = Math.max(30, Math.min(240, Math.round(value)));
  tempo.value = String(next);
  document.getElementById('tempo-up').disabled = next === 240;
  document.getElementById('tempo-down').disabled = next === 30;
  send({type: 'tempo', value: next});
}
tempo.addEventListener('input', () => {
  if (tempo.value !== '' && tempo.validity.valid) setTempo(Number(tempo.value));
});
tempo.addEventListener('change', () => setTempo(Number(tempo.value) || 120));
document.getElementById('tempo-up').addEventListener('click', () => setTempo(Number(tempo.value) + 1));
document.getElementById('tempo-down').addEventListener('click', () => setTempo(Number(tempo.value) - 1));
for (const id of ['trigger', 'play']) {
  document.getElementById(id).addEventListener('click', async () => {
    if (id === 'play' && playing) {
      send({type: 'stop'});
      setPlaying(false);
      return;
    }
    try {
      // Call resume inside the user gesture, before asset loading completes.
      const loading = ensureAudio();
      const resuming = context?.resume();
      await Promise.all([loading, resuming]);
      send({type: id});
      if (id === 'play') setPlaying(true);
    } catch (error) { setStatus(error.message, true); }
  });
}
document.getElementById('stop').addEventListener('click', () => {
  send({type: 'stop'});
  setPlaying(false);
});
document.getElementById('clear').addEventListener('click', () => {
  steps.fill(false);
  [...stepContainer.children].forEach((cell, index) => {
    const button = cell.querySelector('button');
    button.classList.remove('active');
    button.setAttribute('aria-pressed', 'false');
    cell.querySelector('.step-led').classList.remove('lit');
    send({type: 'step', index, active: false});
  });
});
document.getElementById('theme').addEventListener('click', event => {
  const dark = document.documentElement.dataset.theme !== 'dark';
  document.documentElement.dataset.theme = dark ? 'dark' : 'light';
  event.currentTarget.setAttribute('aria-checked', String(dark));
});
document.addEventListener('keydown', event => {
  if (event.code !== 'Space' || event.repeat || event.altKey || event.ctrlKey || event.metaKey ||
      event.target.closest('input, select, textarea, button, [role="slider"], [contenteditable="true"]')) return;
  event.preventDefault();
  document.getElementById('play').click();
});

const downloadButton = document.getElementById('download');
downloadButton.addEventListener('click', async () => {
  if (!machines.length) return;
  downloadButton.disabled = true;
  let worker;
  const name = infoRequest[kind].name;
  setStatus('Rendering sample…');
  try {
    worker = new Worker('sample-worker.js');
    const wav = await new Promise((resolve, reject) => {
      worker.onmessage = ({data}) => data.error ? reject(new Error(data.error)) : resolve(data.wav);
      worker.onerror = () => reject(new Error('Could not render sample. Reload the player and try again.'));
      worker.postMessage({kind, controls: [...controls], level: 127});
    });
    const url = URL.createObjectURL(new Blob([wav], {type: 'audio/wav'}));
    const link = document.createElement('a');
    link.href = url;
    link.download = `${name}.wav`;
    document.body.append(link);
    link.click();
    link.remove();
    setTimeout(() => URL.revokeObjectURL(url), 60000);
    setStatus(`Downloaded ${name}: 44.1 kHz / 24-bit WAV, up to 10 seconds.`);
  } catch (error) { setStatus(error.message, true); }
  finally { worker?.terminate(); downloadButton.disabled = false; }
});

// Prepare the catalog and controls without opening an audio device.
loadInstruments().catch(() => {});
