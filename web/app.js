const canvas = document.getElementById('waterfall');
const ctx = canvas.getContext('2d', {alpha: false});
const orientationButton = document.getElementById('waterfallOrientation');
const waterfallToggle = document.getElementById('waterfallToggle');
let pending = [];
let lastBins = 0;
let waterfallOrientation = 'horizontal';
let waterfallRunning = true;

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

function drawVertical(frame, floor, ceiling) {
  ctx.drawImage(canvas, 0, 0, canvas.width, canvas.height-1, 0, 1, canvas.width, canvas.height-1);
  const row = ctx.createImageData(canvas.width, 1);
  for (let x=0; x<canvas.width; x++) {
    const k = Math.min(frame.bins-1, Math.floor(x * frame.bins / canvas.width));
    const p = Math.max(frame.values[k], 1e-30);
    const db = 10 * Math.log10(p);
    const [r,g,b] = palette((db-floor)/(ceiling-floor));
    const o = 4*x; row.data[o]=r; row.data[o+1]=g; row.data[o+2]=b; row.data[o+3]=255;
  }
  ctx.putImageData(row, 0, 0);
}

function drawHorizontal(frame, floor, ceiling) {
  ctx.drawImage(canvas, 1, 0, canvas.width-1, canvas.height, 0, 0, canvas.width-1, canvas.height);
  const column = ctx.createImageData(1, canvas.height);
  for (let y=0; y<canvas.height; y++) {
    // Frequency runs vertically: highest at the top, lowest at the bottom.
    const k = Math.max(0, frame.bins-1-Math.floor(y * frame.bins / canvas.height));
    const p = Math.max(frame.values[k], 1e-30);
    const db = 10 * Math.log10(p);
    const [r,g,b] = palette((db-floor)/(ceiling-floor));
    const o = 4*y; column.data[o]=r; column.data[o+1]=g; column.data[o+2]=b; column.data[o+3]=255;
  }
  ctx.putImageData(column, canvas.width-1, 0);
}

function drawFrame(frame) {
  if (!waterfallRunning) return;
  const floor = Number(document.getElementById('floor').value);
  const ceiling = Number(document.getElementById('ceiling').value);
  if (waterfallOrientation === 'horizontal') drawHorizontal(frame, floor, ceiling);
  else drawVertical(frame, floor, ceiling);
  updateFrameMetrics(frame);
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
  waterfallToggle.textContent = waterfallRunning ? 'Stop waterfall' : 'Go waterfall';
  waterfallToggle.setAttribute('aria-pressed', waterfallRunning ? 'true' : 'false');
  if (!waterfallRunning) pending.length = 0;
}

function setWaterfallOrientation(orientation) {
  waterfallOrientation = orientation;
  const horizontal = orientation === 'horizontal';
  orientationButton.textContent = `Waterfall: ${orientation}`;
  orientationButton.setAttribute('aria-pressed', horizontal ? 'true' : 'false');
  ctx.clearRect(0, 0, canvas.width, canvas.height);
}

waterfallToggle.addEventListener('click', () => {
  waterfallRunning = !waterfallRunning;
  setWaterfallRunning(waterfallRunning);
});

orientationButton.addEventListener('click', () => {
  setWaterfallOrientation(waterfallOrientation === 'horizontal' ? 'vertical' : 'horizontal');
});

function connectPsd() {
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  const ws = new WebSocket(`${proto}//${location.host}/ws/psd`);
  ws.binaryType = 'arraybuffer';
  ws.onmessage = e => {
    const f = parseFrame(e.data); if (!f) return;
    pending.push(f); if (pending.length > 32) pending.splice(0, pending.length-32);
  };
  ws.onclose = () => { setTimeout(connectPsd, 1000); };
}
connectPsd();

async function refreshConfig() {
  const r = await fetch('/api/config'); const j = await r.json();
  document.getElementById('config').value = j.ok ? j.toml : `ERROR: ${j.error}`;
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
    if (e.key === 'Escape' && state.serverValue !== null) {
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
