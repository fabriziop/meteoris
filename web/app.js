const canvas = document.getElementById('waterfall');
const ctx = canvas.getContext('2d', {alpha: false});
const waterfallStage = document.getElementById('waterfallStage');
const frequencyScale = document.getElementById('frequencyScale');
const orientationButton = document.getElementById('waterfallOrientation');
const waterfallToggle = document.getElementById('waterfallToggle');
const waterfallSound = document.getElementById('waterfallSound');
const bandMinInput = document.getElementById('bandMin');
const bandMaxInput = document.getElementById('bandMax');
let pending = [];
let lastBins = 0;
let waterfallOrientation = 'horizontal';
let waterfallRunning = true;
let configuredBandwidthHz = null;
let bandMinHz = null;
let bandMaxHz = null;
let floorDb = -130;
let ceilingDb = -60;

// A PSD has power but no phase, so receiver audio cannot be reconstructed.
// Instead, sonify the visible spectrum with one oscillator per frequency band.
// The selected RF-offset range is mapped linearly onto 80 Hz .. 5 kHz.
const AUDIO_BANDS = 48;
const AUDIO_MIN_HZ = 80;
const AUDIO_MAX_HZ = 5000;
const AUDIO_GATE_DB = 10;
const AUDIO_FULL_SCALE_DB = 24;
let audioContext = null;
let audioMaster = null;
let audioVoices = [];
let audioEnabled = false;
let lastAudioUpdateMs = 0;

function createAudioGraph() {
  if (audioContext) return;
  const AudioContextClass = window.AudioContext || window.webkitAudioContext;
  if (!AudioContextClass) throw new Error('Web Audio is not supported by this browser');
  audioContext = new AudioContextClass();
  audioMaster = audioContext.createGain();
  audioMaster.gain.setValueAtTime(0, audioContext.currentTime);
  audioMaster.connect(audioContext.destination);
  audioVoices = [];
  for (let i = 0; i < AUDIO_BANDS; i++) {
    const oscillator = audioContext.createOscillator();
    const gain = audioContext.createGain();
    const position = AUDIO_BANDS === 1 ? 0 : i / (AUDIO_BANDS - 1);
    oscillator.frequency.value = AUDIO_MIN_HZ + position * (AUDIO_MAX_HZ - AUDIO_MIN_HZ);
    gain.gain.value = 0;
    oscillator.connect(gain);
    gain.connect(audioMaster);
    oscillator.start();
    audioVoices.push({oscillator, gain});
  }
}

async function setAudioEnabled(enabled) {
  if (enabled) {
    createAudioGraph();
    await audioContext.resume();
    audioEnabled = true;
    audioMaster.gain.cancelScheduledValues(audioContext.currentTime);
    audioMaster.gain.setTargetAtTime(0.22, audioContext.currentTime, 0.03);
  } else {
    audioEnabled = false;
    if (audioContext) {
      audioMaster.gain.cancelScheduledValues(audioContext.currentTime);
      audioMaster.gain.setTargetAtTime(0, audioContext.currentTime, 0.02);
      window.setTimeout(() => {
        if (!audioEnabled && audioContext?.state === 'running') audioContext.suspend();
      }, 120);
    }
  }
  waterfallSound.textContent = audioEnabled ? 'Mute' : 'Sound';
  waterfallSound.setAttribute('aria-pressed', audioEnabled ? 'true' : 'false');
}

function updatePsdAudio(frame) {
  if (!audioEnabled || !audioContext || audioContext.state !== 'running') return;
  const nowMs = performance.now();
  if (nowMs - lastAudioUpdateMs < 30) return;
  lastAudioUpdateMs = nowMs;
  const range = visibleBinRange(frame);
  const binCount = Math.max(1, range.last - range.first + 1);
  const now = audioContext.currentTime;
  const sortedPower = [];
  for (let k = range.first; k <= range.last; k++) {
    sortedPower.push(Math.max(frame.values[k], 1e-30));
  }
  sortedPower.sort((a, b) => a - b);
  const noisePower = sortedPower[Math.floor(sortedPower.length / 2)] || 1e-30;
  const noiseDb = 10 * Math.log10(noisePower);
  const weights = [];
  let weightSum = 0;
  for (let band = 0; band < AUDIO_BANDS; band++) {
    const first = range.first + Math.floor(band * binCount / AUDIO_BANDS);
    const last = Math.min(range.last,
      range.first + Math.floor((band + 1) * binCount / AUDIO_BANDS) - 1);
    let peakPower = 1e-30;
    for (let k = first; k <= Math.max(first, last); k++) {
      peakPower = Math.max(peakPower, frame.values[k]);
    }
    const excessDb = 10 * Math.log10(Math.max(peakPower, 1e-30)) - noiseDb;
    const level = Math.max(0, Math.min(1,
      (excessDb - AUDIO_GATE_DB) / (AUDIO_FULL_SCALE_DB - AUDIO_GATE_DB)));
    const weight = level * level;
    weights.push(weight);
    weightSum += weight;
  }
  // Keep one narrow signal clearly audible without allowing several strong
  // bands to add up into clipping. Noise-only bands remain below the gate.
  const normalization = 0.35 / Math.max(1, weightSum);
  for (let band = 0; band < AUDIO_BANDS; band++) {
    const amplitude = weights[band] * normalization;
    audioVoices[band].gain.gain.setTargetAtTime(amplitude, now, 0.025);
  }
}

function palette(t) {
  // Match meteoris_plot.gqrx_colormap(): classic 256-entry Gqrx-like palette.
  const i = Math.max(0, Math.min(255, Math.round(255 * t)));
  let r, g, b;
  if (i < 20) {
    r = 0; g = 0; b = 0;
  } else if (i < 70) {
    r = 0; g = 0; b = 140 * (i - 20) / 50;
  } else if (i < 100) {
    r = 60 * (i - 70) / 30;
    g = 125 * (i - 70) / 30;
    b = 115 * (i - 70) / 30 + 140;
  } else if (i < 150) {
    r = 195 * (i - 100) / 50 + 60;
    g = 130 * (i - 100) / 50 + 125;
    b = 255 - 255 * (i - 100) / 50;
  } else if (i < 250) {
    r = 255;
    g = 255 - 255 * (i - 150) / 100;
    b = 0;
  } else {
    r = 255;
    g = 255 * (i - 250) / 5;
    b = 255 * (i - 250) / 5;
  }
  return [Math.round(r), Math.round(g), Math.round(b)];
}

function parseFrame(buf) {
  const v = new DataView(buf);
  if (String.fromCharCode(v.getUint8(0),v.getUint8(1),v.getUint8(2),v.getUint8(3)) !== 'MPSD') return null;
  const bins = v.getUint32(12, true);
  const headerBytes = v.getUint16(6, true);
  const f = new Float32Array(buf, headerBytes, bins);
  return {
    bins, values: f,
    index: Number(v.getBigUint64(16, true)),
    timestampNs: v.getBigUint64(24, true),
    gainDb: v.getFloat32(40, true),
    centerHz: v.getFloat64(44, true),
    startHz: v.getFloat64(52, true),
    stepHz: v.getFloat64(60, true)
  };
}

function updateFrameMetrics(frame) {
  document.getElementById('frequency').textContent = `${(frame.centerHz/1e6).toFixed(6)} MHz`;
  document.getElementById('gain').textContent = `${frame.gainDb.toFixed(1)} dB`;
  document.getElementById('frame').textContent = `PSD #${frame.index}`;
  if (lastBins !== frame.bins) lastBins = frame.bins;
}

function parseConfiguredBandwidth(toml) {
  let inDsp = false;
  for (const rawLine of toml.split(/\r?\n/)) {
    const line = rawLine.replace(/#.*/, '').trim();
    if (!line) continue;
    const section = line.match(/^\[([^\]]+)\]$/);
    if (section) {
      inDsp = section[1].trim() === 'dsp';
      continue;
    }
    if (!inDsp) continue;
    const match = line.match(/^bandwidth_hz\s*=\s*([+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?)$/);
    if (match) return Number(match[1]);
  }
  return null;
}

function initializeBandControls(bandwidthHz) {
  if (!(bandwidthHz > 0)) return;
  configuredBandwidthHz = bandwidthHz;
  bandMinHz = -bandwidthHz / 2;
  bandMaxHz = bandwidthHz / 2;
  bandMinInput.min = String(bandMinHz);
  bandMinInput.max = String(bandMaxHz);
  bandMaxInput.min = String(bandMinHz);
  bandMaxInput.max = String(bandMaxHz);
  bandMinInput.value = String(bandMinHz);
  bandMaxInput.value = String(bandMaxHz);
  updateFrequencyScale();
}

function ensureBandFromFrame(frame) {
  if (bandMinHz !== null && bandMaxHz !== null) return;
  const first = frame.startHz;
  const last = frame.startHz + frame.stepHz * Math.max(0, frame.bins - 1);
  const lo = Math.min(first, last);
  const hi = Math.max(first, last);
  configuredBandwidthHz = hi - lo;
  bandMinHz = lo;
  bandMaxHz = hi;
  bandMinInput.min = String(lo);
  bandMinInput.max = String(hi);
  bandMaxInput.min = String(lo);
  bandMaxInput.max = String(hi);
  bandMinInput.value = String(lo);
  bandMaxInput.value = String(hi);
  updateFrequencyScale();
}

function applyBandInputs() {
  if (!(configuredBandwidthHz > 0)) return;
  const fullMin = -configuredBandwidthHz / 2;
  const fullMax = configuredBandwidthHz / 2;
  let lo = Number(bandMinInput.value);
  let hi = Number(bandMaxInput.value);
  if (!Number.isFinite(lo)) lo = bandMinHz;
  if (!Number.isFinite(hi)) hi = bandMaxHz;
  lo = Math.max(fullMin, Math.min(fullMax, lo));
  hi = Math.max(fullMin, Math.min(fullMax, hi));
  if (lo >= hi) {
    bandMinInput.value = String(bandMinHz);
    bandMaxInput.value = String(bandMaxHz);
    return;
  }
  bandMinHz = lo;
  bandMaxHz = hi;
  bandMinInput.value = String(lo);
  bandMaxInput.value = String(hi);
  ctx.clearRect(0, 0, canvas.width, canvas.height);
  updateFrequencyScale();
}


function visibleBinRange(frame) {
  ensureBandFromFrame(frame);
  const first = frame.startHz;
  const step = frame.stepHz;
  if (!Number.isFinite(step) || step === 0) return {first: 0, last: frame.bins - 1};
  const a = Math.ceil((bandMinHz - first) / step);
  const b = Math.floor((bandMaxHz - first) / step);
  return {
    first: Math.max(0, Math.min(frame.bins - 1, Math.min(a, b))),
    last: Math.max(0, Math.min(frame.bins - 1, Math.max(a, b)))
  };
}

function drawVertical(frame, floor, ceiling) {
  const range = visibleBinRange(frame);
  const count = Math.max(1, range.last - range.first + 1);
  ctx.drawImage(canvas, 0, 0, canvas.width, canvas.height-1, 0, 1, canvas.width, canvas.height-1);
  const row = ctx.createImageData(canvas.width, 1);
  for (let x=0; x<canvas.width; x++) {
    const k = Math.min(range.last, range.first + Math.floor(x * count / canvas.width));
    const p = Math.max(frame.values[k], 1e-30);
    const db = 10 * Math.log10(p);
    const [r,g,b] = palette((db-floor)/(ceiling-floor));
    const o = 4*x; row.data[o]=r; row.data[o+1]=g; row.data[o+2]=b; row.data[o+3]=255;
  }
  ctx.putImageData(row, 0, 0);
}

function drawHorizontal(frame, floor, ceiling) {
  const range = visibleBinRange(frame);
  const count = Math.max(1, range.last - range.first + 1);
  ctx.drawImage(canvas, 1, 0, canvas.width-1, canvas.height, 0, 0, canvas.width-1, canvas.height);
  const column = ctx.createImageData(1, canvas.height);
  for (let y=0; y<canvas.height; y++) {
    // Frequency runs vertically: highest at the top, lowest at the bottom.
    const k = Math.max(range.first, range.last - Math.floor(y * count / canvas.height));
    const p = Math.max(frame.values[k], 1e-30);
    const db = 10 * Math.log10(p);
    const [r,g,b] = palette((db-floor)/(ceiling-floor));
    const o = 4*y; column.data[o]=r; column.data[o+1]=g; column.data[o+2]=b; column.data[o+3]=255;
  }
  ctx.putImageData(column, canvas.width-1, 0);
}

function drawFrame(frame) {
  if (!waterfallRunning) return;
  ensureBandFromFrame(frame);
  if (waterfallOrientation === 'horizontal') drawHorizontal(frame, floorDb, ceilingDb);
  else drawVertical(frame, floorDb, ceilingDb);
  updateFrameMetrics(frame);
}

function formatOffset(hz) {
  if (Math.abs(hz) >= 1e6) return `${(hz / 1e6).toFixed(3)}M`;
  if (Math.abs(hz) >= 1e3) return `${(hz / 1e3).toFixed(1)}k`;
  return `${Math.round(hz)}`;
}

function scaleTickValues(lo, hi) {
  const values = [];
  const count = 5;
  for (let i = 0; i < count; i++) values.push(lo + (hi - lo) * i / (count - 1));
  if (lo < 0 && hi > 0 && !values.some(v => Math.abs(v) < 1e-9)) {
    let nearest = 1;
    for (let i = 2; i < values.length - 1; i++) {
      if (Math.abs(values[i]) < Math.abs(values[nearest])) nearest = i;
    }
    values[nearest] = 0;
    values.sort((a, b) => a - b);
  }
  return values;
}

function scaleTickMarks(lo, hi) {
  const majorValues = scaleTickValues(lo, hi);
  const marks = [];
  for (let i = 0; i < majorValues.length; i++) {
    marks.push({value: majorValues[i], kind: 'major'});
    if (i === majorValues.length - 1) continue;
    const a = majorValues[i];
    const b = majorValues[i + 1];
    for (let subdivision = 1; subdivision < 4; subdivision++) {
      marks.push({
        value: a + (b - a) * subdivision / 4,
        kind: subdivision === 2 ? 'medium' : 'minor',
      });
    }
  }
  return marks.sort((a, b) => a.value - b.value);
}

function updateFrequencyScale() {
  if (bandMinHz === null || bandMaxHz === null || bandMaxHz <= bandMinHz) return;
  frequencyScale.replaceChildren();
  const span = bandMaxHz - bandMinHz;
  for (const mark of scaleTickMarks(bandMinHz, bandMaxHz)) {
    const tick = document.createElement('span');
    tick.className = `frequency-tick ${mark.kind}`;
    if (mark.kind === 'major') tick.textContent = formatOffset(mark.value);
    const position = (mark.value - bandMinHz) / span * 100;
    if (waterfallOrientation === 'vertical') {
      tick.style.left = `${position}%`;
      if (mark.kind === 'major' && position <= 0.000001) tick.classList.add('edge-start');
      if (mark.kind === 'major' && position >= 99.999999) tick.classList.add('edge-end');
    } else {
      tick.style.top = `${100 - position}%`;
    }
    frequencyScale.appendChild(tick);
  }
}

function animate() {
  if (waterfallRunning) {
    const batch = pending.splice(0, 8);
    for (const f of batch) drawFrame(f);
  }
  requestAnimationFrame(animate);
}
requestAnimationFrame(animate);

function setWaterfallRunning(running) {
  waterfallRunning = running;
  waterfallToggle.textContent = waterfallRunning ? 'Stop' : 'Go';
  waterfallToggle.setAttribute('aria-pressed', waterfallRunning ? 'true' : 'false');
  if (!waterfallRunning) pending.length = 0;
}

function setWaterfallOrientation(orientation) {
  waterfallOrientation = orientation;
  const horizontal = orientation === 'horizontal';
  orientationButton.textContent = horizontal ? 'Vertical' : 'Horizontal';
  orientationButton.setAttribute('aria-pressed', horizontal ? 'true' : 'false');
  waterfallStage.classList.toggle('horizontal', horizontal);
  waterfallStage.classList.toggle('vertical', !horizontal);
  ctx.clearRect(0, 0, canvas.width, canvas.height);
  updateFrequencyScale();
}

waterfallToggle.addEventListener('click', () => {
  waterfallRunning = !waterfallRunning;
  setWaterfallRunning(waterfallRunning);
});

orientationButton.addEventListener('click', () => {
  setWaterfallOrientation(waterfallOrientation === 'horizontal' ? 'vertical' : 'horizontal');
});

waterfallSound.addEventListener('click', async () => {
  try {
    await setAudioEnabled(!audioEnabled);
  } catch (err) {
    audioEnabled = false;
    waterfallSound.textContent = 'Unavailable';
    waterfallSound.disabled = true;
    waterfallSound.title = err.message;
  }
});

function applyLevelInputs() {
  const floorInput = document.getElementById('floor');
  const ceilingInput = document.getElementById('ceiling');
  const nextFloor = Number(floorInput.value);
  const nextCeiling = Number(ceilingInput.value);
  if (!Number.isFinite(nextFloor) || !Number.isFinite(nextCeiling) || nextFloor >= nextCeiling) {
    floorInput.value = String(floorDb);
    ceilingInput.value = String(ceilingDb);
    return;
  }
  floorDb = nextFloor;
  ceilingDb = nextCeiling;
}

function commitNumberInput(input, apply) {
  // A number input's spinner can still have a pending native edit while the
  // Enter keydown handler runs. Blurring commits that edit; waiting one frame
  // then makes sure apply() reads the value displayed by the control.
  input.blur();
  window.requestAnimationFrame(apply);
}

// Some browsers leave focus on the previously clicked button when the native
// stepper arrows of an input[type=number] are used. Make the number input the
// keyboard target explicitly so Enter reaches its commit handler.
for (const input of document.querySelectorAll('input[type="number"]')) {
  input.addEventListener('pointerdown', () => {
    input.focus({preventScroll: true});
  });
}

for (const input of [document.getElementById('floor'), document.getElementById('ceiling')]) {
  input.addEventListener('keydown', e => {
    if (e.key === 'Enter') {
      e.preventDefault();
      commitNumberInput(input, applyLevelInputs);
    }
  });
}

for (const input of [bandMinInput, bandMaxInput]) {
  input.addEventListener('keydown', e => {
    if (e.key === 'Enter') {
      e.preventDefault();
      commitNumberInput(input, applyBandInputs);
    }
  });
}

function connectPsd() {
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  const ws = new WebSocket(`${proto}//${location.host}/ws/psd`);
  ws.binaryType = 'arraybuffer';
  ws.onmessage = e => {
    const f = parseFrame(e.data); if (!f) return;
    updatePsdAudio(f);
    pending.push(f); if (pending.length > 32) pending.splice(0, pending.length-32);
  };
  ws.onclose = () => { setTimeout(connectPsd, 1000); };
}
connectPsd();

async function refreshConfig() {
  const r = await fetch('/api/config'); const j = await r.json();
  document.getElementById('config').value = j.ok ? j.toml : `ERROR: ${j.error}`;
  if (j.ok) {
    const bandwidth = parseConfiguredBandwidth(j.toml);
    if (bandwidth > 0) initializeBandControls(bandwidth);
  }
}
const liveInputs = {
  centerFrequency: {serverValue: null, dirty: false},
  gainControl: {serverValue: null, dirty: false},
};

function updateLiveInput(id, value) {
  const input = document.getElementById(id);
  const state = liveInputs[id];
  state.serverValue = value;
  // Never destroy an in-progress user edit. Polling may continue to update
  // serverValue in the background; the draft is synchronized after Apply,
  // or when Escape explicitly cancels the edit.
  if (!state.dirty && document.activeElement !== input) input.value = value;
}

for (const [id, state] of Object.entries(liveInputs)) {
  const input = document.getElementById(id);
  input.addEventListener('input', () => { state.dirty = true; });
  input.addEventListener('keydown', e => {
    if (e.key === 'Enter') {
      e.preventDefault();
      commitNumberInput(input, () => {
        document.querySelector(`button[data-input="${id}"]`)?.click();
      });
    } else if (e.key === 'Escape' && state.serverValue !== null) {
      input.value = state.serverValue;
      state.dirty = false;
      input.blur();
    }
  });
}

let sessionListenIp = '';
let sessionVersion = '';

function setDspStatus(connected) {
  const badge = document.getElementById('statusBadge');
  const suffix = `to meteoris${sessionVersion ? ` ${sessionVersion}` : ''}${sessionListenIp ? ` - ip ${sessionListenIp}` : ''}`;
  badge.textContent = `${connected ? 'connected' : 'connecting'} ${suffix}`;
  badge.classList.toggle('connected', connected);
  badge.classList.toggle('connecting', !connected);
}

async function refreshSessionInfo() {
  const r = await fetch('/api/session');
  const j = await r.json();
  if (!j.ok) throw new Error(j.error || 'cannot read web session information');
  sessionListenIp = j.listen_ip || '';
  sessionVersion = j.version || '';
  setDspStatus(false);
}

async function refreshStatus() {
  try {
    const r = await fetch('/api/status'); const j = await r.json();
    if (j.ok) {
      updateLiveInput('centerFrequency', j.status.center_frequency);
      updateLiveInput('gainControl', j.status.gain_db);
      setDspStatus(true);
    } else {
      setDspStatus(false);
    }
  } catch (err) {
    setDspStatus(false);
  }
}
for (const b of document.querySelectorAll('button[data-set]')) b.onclick = async () => {
  const inputId = b.dataset.input;
  const input = document.getElementById(inputId);
  const parameter = b.dataset.set, value = Number(input.value);
  const r = await fetch('/api/set', {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({parameter,value})});
  const j = await r.json(); document.getElementById('controlMessage').textContent = j.ok ? j.result : j.error;
  if (j.ok) liveInputs[inputId].dirty = false;
  await refreshStatus();
};
refreshConfig();
refreshSessionInfo()
  .catch(() => setDspStatus(false))
  .finally(() => { refreshStatus(); setInterval(refreshStatus, 2000); });
