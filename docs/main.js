// main.js — input, camera, collision and the frame pipeline that drives the
// C++/WebAssembly ray tracer across a pool of Web Workers. No rendering logic
// lives here: every pixel is produced by the C++ code in ../src.
'use strict';

const $ = (id) => document.getElementById(id);
const canvas = $('view');
const ctx = canvas.getContext('2d', { alpha: false });
const low = document.createElement('canvas');            // internal render target
const lctx = low.getContext('2d', { alpha: false });

const DEG = Math.PI / 180;
const EYE = 2.2;                                          // island top (0.5) + 1.7 m
const START = { x: 9.5, z: 15.5 };

const state = {
  x: START.x, z: START.z, eye: EYE,
  yaw: Math.atan2(-START.z, -START.x) + 0.12, pitch: 0.07, fov: 70 * DEG,
  drone: false, tour: false, tourAngle: Math.atan2(START.z, START.x),
  time: 0, paused: false,
  sunElev: 7 * DEG, sunAz: 2.3, exposure: 1.0,
  quality: 'balanced', bloom: true, denoise: true, shadows: true,
  scale: 0.45, autoRes: true,
};

const QUALITY = {
  performance: { target: 24, giStill: 0.0, giMove: 0.0, maxW: 960 },
  balanced:    { target: 36, giStill: 0.7, giMove: 0.2, maxW: 1280 },
  photo:       { target: 1e9, giStill: 1.0, giMove: 1.0, maxW: 1920 },
};

const PRESETS = {
  golden: { elev: 7,   az: 2.3 },
  noon:   { elev: 58,  az: 1.9 },
  blue:   { elev: -4,  az: 2.6 },
  night:  { elev: -14, az: 2.6 },
};

// ── worker pool ─────────────────────────────────────────────────────────────
const threads = Math.max(2, Math.min(16, navigator.hardwareConcurrency || 4));
const workers = [];
let ready = 0;

async function boot() {
  if (typeof WebAssembly !== 'object') return fail('This browser does not support WebAssembly.');
  setLoading('Downloading C++ ray tracer…', 0.1);
  let module;
  try {
    const bytes = await (await fetch('raytracer.wasm')).arrayBuffer();
    setLoading('Compiling WebAssembly…', 0.35);
    module = await WebAssembly.compile(bytes);
  } catch (err) {
    return fail('Could not load raytracer.wasm (' + err + '). If you opened the file directly, serve the folder over HTTP.');
  }
  setLoading(`Building BVH on ${threads} threads…`, 0.6);
  for (let i = 0; i < threads; i++) {
    const w = new Worker('worker.js');
    w.onmessage = onWorkerMessage;
    w.onerror = (e) => fail('Worker error: ' + e.message);
    w.postMessage({ type: 'init', module, index: i, count: threads });
    workers.push(w);
  }
}

function onWorkerMessage(e) {
  const m = e.data;
  if (m.type === 'ready') {
    ready++;
    setLoading(`Starting render threads ${ready}/${threads}…`, 0.6 + 0.4 * ready / threads);
    if (ready === threads) start();
  } else if (m.type === 'rows') {
    receiveRows(m);
  } else if (m.type === 'error') {
    fail(m.message);
  }
}

// ── frame pipeline ──────────────────────────────────────────────────────────
let img = null, img32 = null, dimg = null, dimg32 = null, rw = 0, rh = 0;
let busy = false, pending = 0, frameNo = 0, dispatchT = 0, workerMs = 0;
let accumN = 0, lastKey = '';
let fps = 0, lastPresent = performance.now();

function renderSize() {
  const q = QUALITY[state.quality];
  const scale = state.quality === 'photo' ? 1 : state.scale;
  let w = Math.max(160, Math.round(canvas.width * scale));
  w = Math.min(w, q.maxW);
  const h = Math.max(90, Math.round(w * canvas.height / canvas.width));
  return [w, h];
}

function dispatch() {
  const [w, h] = renderSize();
  if (w !== rw || h !== rh) {
    rw = w; rh = h;
    low.width = w; low.height = h;
    img = lctx.createImageData(w, h);
    img32 = new Uint32Array(img.data.buffer);
    dimg = lctx.createImageData(w, h);
    dimg32 = new Uint32Array(dimg.data.buffer);
    accumN = 0;
  }
  const cam = [state.x, state.eye, state.z, state.yaw, state.pitch];
  const timeFrozen = state.paused || state.quality === 'photo';
  const key = cam.map((v) => v.toFixed(4)).join() + `|${w}x${h}|${state.sunElev}|${state.sunAz}|${state.shadows}|${state.quality}` +
              (timeFrozen ? `|${state.time.toFixed(3)}` : '');
  const still = key === lastKey;
  lastKey = key;
  accumN = still ? accumN + 1 : 1;

  // Progressive refinement: a true running average while the scene is frozen,
  // a short temporal window (EMA) while cars and waves are moving.
  let blend = 1 / accumN;
  if (!timeFrozen) blend = Math.max(blend, 0.1);
  const q = QUALITY[state.quality];
  const gi = still ? q.giStill : q.giMove;

  busy = true;
  pending = workers.length;
  dispatchT = performance.now();
  workerMs = 0;
  frameNo++;
  for (const wk of workers) {
    wk.postMessage({
      type: 'frame', w, h, time: state.time, cam, fov: state.fov, frame: frameNo, blend,
      sunElev: state.sunElev, sunAz: state.sunAz, exposure: state.exposure, gi, shadows: state.shadows,
    });
  }
}

function receiveRows(m) {
  if (m.w === rw && m.h === rh) {
    const src = new Uint32Array(m.buf);
    for (let i = 0; i < m.rows; i++) {
      const y = m.index + i * threads;
      img32.set(src.subarray(i * m.w, (i + 1) * m.w), y * m.w);
    }
  }
  workerMs = Math.max(workerMs, m.ms);
  if (--pending === 0) present();
}

// Edge-aware (bilateral) denoise on the 8-bit display image. Each pixel becomes a weighted
// average of its 5×5 neighbourhood; the weight falls off with spatial distance and, more
// steeply, with colour difference, so noise averages out while real edges stay sharp.
// Only the display copy is filtered — the float accumulator in WASM is untouched.
const DEN_BASE_SIGMA = 28;                                // colour tolerance in 8-bit units
const DEN_SPATIAL = new Float32Array(25);
for (let dy = -2; dy <= 2; dy++)
  for (let dx = -2; dx <= 2; dx++) DEN_SPATIAL[(dy + 2) * 5 + dx + 2] = Math.exp(-(dx * dx + dy * dy) / (2 * 1.5 * 1.5));
const denLut = new Float32Array(2048);                    // exp(-d²/2σ²), indexed by d² >> 5
let denLutSigma = -1;

function denoise(src, dst, w, h, sigma) {
  if (sigma !== denLutSigma) {
    denLutSigma = sigma;
    for (let i = 0; i < denLut.length; i++) denLut[i] = Math.exp(-(i * 32) / (2 * sigma * sigma));
  }
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const c = src[y * w + x];
      const cr = c & 255, cg = (c >> 8) & 255, cb = (c >> 16) & 255;
      let sr = 0, sg = 0, sb = 0, sw = 0, k = 0;
      for (let dy = -2; dy <= 2; dy++) {
        const yy = Math.min(h - 1, Math.max(0, y + dy)) * w;
        for (let dx = -2; dx <= 2; dx++, k++) {
          const p = src[yy + Math.min(w - 1, Math.max(0, x + dx))];
          const r = p & 255, g = (p >> 8) & 255, b = (p >> 16) & 255;
          const dr = r - cr, dg = g - cg, db = b - cb;
          const wt = DEN_SPATIAL[k] * denLut[Math.min(2047, (dr * dr + dg * dg + db * db) >> 5)];
          sr += r * wt; sg += g * wt; sb += b * wt; sw += wt;
        }
      }
      const inv = 1 / sw;                                 // centre tap has weight 1, so sw ≥ 1
      dst[y * w + x] = 0xFF000000 | (((sb * inv + 0.5) | 0) << 16) | (((sg * inv + 0.5) | 0) << 8) | ((sr * inv + 0.5) | 0);
    }
  }
}

function present() {
  // Strength fades as a frozen/still image converges; while time runs the temporal window stays short, so keep it full.
  const frozen = state.paused || state.quality === 'photo';
  const strength = frozen ? 1 / Math.sqrt(Math.max(1, accumN)) : 1;
  if (state.denoise && strength > 0.15) {
    denoise(img32, dimg32, rw, rh, DEN_BASE_SIGMA * strength);
    lctx.putImageData(dimg, 0, 0);
  } else {
    lctx.putImageData(img, 0, 0);
  }
  ctx.imageSmoothingEnabled = true;
  ctx.imageSmoothingQuality = 'high';
  ctx.globalCompositeOperation = 'source-over';
  ctx.globalAlpha = 1;
  ctx.filter = 'none';
  ctx.drawImage(low, 0, 0, canvas.width, canvas.height);
  if (state.bloom) {                                       // cheap glow pass for neon and sun
    ctx.globalCompositeOperation = 'lighter';
    ctx.globalAlpha = 0.16;
    ctx.filter = `blur(${Math.round(canvas.width / 90)}px) brightness(1.15) contrast(1.6)`;
    ctx.drawImage(low, 0, 0, canvas.width, canvas.height);
    ctx.filter = 'none';
    ctx.globalAlpha = 1;
    ctx.globalCompositeOperation = 'source-over';
  }

  const now = performance.now();
  const frameMs = now - dispatchT;
  fps = fps * 0.85 + (1000 / Math.max(1, now - lastPresent)) * 0.15;
  lastPresent = now;
  adaptResolution(frameMs);
  updateStats(frameMs);
  busy = false;
}

function adaptResolution(frameMs) {
  if (!state.autoRes || state.quality === 'photo' || accumN > 1) return;   // never resize mid-accumulation
  const target = QUALITY[state.quality].target;
  if (frameMs > target * 1.25) state.scale *= 0.92;
  else if (frameMs < target * 0.7) state.scale *= 1.04;
  state.scale = Math.min(1, Math.max(0.18, state.scale));
  $('res').value = Math.round(state.scale * 100);
}

// ── input ───────────────────────────────────────────────────────────────────
const keys = new Set();
addEventListener('keydown', (e) => {
  if (e.target.tagName === 'INPUT' || e.target.tagName === 'SELECT') return;
  keys.add(e.code);
  if (['KeyW', 'KeyA', 'KeyS', 'KeyD', 'ArrowUp', 'ArrowDown'].includes(e.code)) state.tour = false;
  if (e.code === 'KeyT') toggleTour();
  if (e.code === 'KeyV') toggleDrone();
  if (e.code === 'KeyP') setQuality(state.quality === 'photo' ? 'balanced' : 'photo');
  if (e.code === 'KeyH') document.body.classList.toggle('hide-ui');
  if (e.code === 'Space') { state.paused = !state.paused; $('pause').checked = state.paused; }
  if (e.code === 'KeyF') toggleFullscreen();
  if (e.code === 'Space' || e.code.startsWith('Arrow')) e.preventDefault();
});
addEventListener('keyup', (e) => keys.delete(e.code));
addEventListener('blur', () => keys.clear());

canvas.addEventListener('click', () => {
  $('start').classList.add('gone');
  if (!('ontouchstart' in window) && canvas.requestPointerLock) canvas.requestPointerLock();
});
addEventListener('mousemove', (e) => {
  if (document.pointerLockElement !== canvas) return;
  look(e.movementX * 0.0022, -e.movementY * 0.0022);
});
function look(dyaw, dpitch) {
  state.yaw += dyaw;
  state.pitch = Math.max(-1.35, Math.min(1.35, state.pitch + dpitch));
}

// Touch: left half = move stick, right half = look.
const touches = new Map();
canvas.addEventListener('touchstart', (e) => {
  $('start').classList.add('gone');
  for (const t of e.changedTouches)
    touches.set(t.identifier, { x0: t.clientX, y0: t.clientY, x: t.clientX, y: t.clientY, move: t.clientX < innerWidth / 2 });
  e.preventDefault();
}, { passive: false });
canvas.addEventListener('touchmove', (e) => {
  for (const t of e.changedTouches) {
    const s = touches.get(t.identifier); if (!s) continue;
    if (!s.move) look((t.clientX - s.x) * 0.005, -(t.clientY - s.y) * 0.005);
    s.x = t.clientX; s.y = t.clientY;
  }
  e.preventDefault();
}, { passive: false });
const endTouch = (e) => { for (const t of e.changedTouches) touches.delete(t.identifier); };
canvas.addEventListener('touchend', endTouch);
canvas.addEventListener('touchcancel', endTouch);

// ── movement & collision ────────────────────────────────────────────────────
const CIRCLES = [[-9.5, 7, 0.45], [6, 7, 0.45], [6, -6.6, 0.45]];
for (let k = 0; k < 6; k++) { const a = (30 + 60 * k) * DEG; CIRCLES.push([15.5 * Math.cos(a), 15.5 * Math.sin(a), 0.55]); }
for (let k = 0; k < 8; k++) { const a = (22.5 + 45 * k) * DEG; CIRCLES.push([13.8 * Math.cos(a), 13.8 * Math.sin(a), 0.3]); }
const HOUSE = { x0: -8.45, x1: 8.3, z0: -6.45, z1: 6.3 };   // glass walls + body radius

function collide(p, prev) {
  if (state.drone) {
    const r = Math.hypot(p.x, p.z);
    if (r > 70) { p.x *= 70 / r; p.z *= 70 / r; }
    return;
  }
  for (const [cx, cz, r] of CIRCLES) {
    const dx = p.x - cx, dz = p.z - cz, d = Math.hypot(dx, dz);
    if (d < r && d > 1e-6) { p.x = cx + dx / d * r; p.z = cz + dz / d * r; }
  }
  if (p.x > HOUSE.x0 && p.x < HOUSE.x1 && p.z > HOUSE.z0 && p.z < HOUSE.z1) {
    const pen = [p.x - HOUSE.x0, HOUSE.x1 - p.x, p.z - HOUSE.z0, HOUSE.z1 - p.z];
    const i = pen.indexOf(Math.min(...pen));
    if (i === 0) p.x = HOUSE.x0; else if (i === 1) p.x = HOUSE.x1; else if (i === 2) p.z = HOUSE.z0; else p.z = HOUSE.z1;
  }
  const prevOnDock = Math.hypot(prev.x, prev.z) > 23.35;
  const r = Math.hypot(p.x, p.z);
  if (prevOnDock || (r > 23.3 && p.z > 0 && Math.abs(p.x) < 1.2)) {       // boardwalk
    p.x = Math.max(-1.2, Math.min(1.2, p.x));
    p.z = Math.min(33.4, p.z);
    if (Math.hypot(p.x, p.z) < 23.3 && !prevOnDock) return;
  } else if (r > 23.3) { p.x *= 23.3 / r; p.z *= 23.3 / r; }
}

function update(dt) {
  if (!state.paused && state.quality !== 'photo') state.time += dt;

  if (state.tour) {
    state.tourAngle -= dt * 0.11;
    const R = 15.2;
    state.x = R * Math.cos(state.tourAngle);
    state.z = R * Math.sin(state.tourAngle);
    const targetYaw = Math.atan2(-state.z, -state.x) + 0.35;
    let dy = targetYaw - state.yaw;
    dy = Math.atan2(Math.sin(dy), Math.cos(dy));
    state.yaw += dy * Math.min(1, dt * 2);
    state.pitch += (0.12 - state.pitch) * Math.min(1, dt * 2);
  } else {
    const fx = Math.cos(state.yaw), fz = Math.sin(state.yaw);
    const rx = -fz, rz = fx;
    let f = 0, s = 0;
    if (keys.has('KeyW') || keys.has('ArrowUp')) f++;
    if (keys.has('KeyS') || keys.has('ArrowDown')) f--;
    if (keys.has('KeyD')) s++;
    if (keys.has('KeyA')) s--;
    for (const t of touches.values()) if (t.move) { f -= (t.y - t.y0) / 60; s += (t.x - t.x0) / 60; }
    f = Math.max(-1, Math.min(1, f)); s = Math.max(-1, Math.min(1, s));
    if (keys.has('ArrowLeft')) look(-1.6 * dt, 0);
    if (keys.has('ArrowRight')) look(1.6 * dt, 0);
    const speed = (keys.has('ShiftLeft') || keys.has('ShiftRight') ? 7 : 3.2) * (state.drone ? 2.5 : 1);
    const prev = { x: state.x, z: state.z };
    const p = { x: state.x + (fx * f + rx * s) * speed * dt, z: state.z + (fz * f + rz * s) * speed * dt };
    collide(p, prev);
    state.x = p.x; state.z = p.z;
  }
  const targetEye = state.drone ? 11 : EYE;
  state.eye += (targetEye - state.eye) * Math.min(1, dt * 3);
  if (Math.abs(targetEye - state.eye) < 0.002) state.eye = targetEye;
}

function toggleTour() { state.tour = !state.tour; if (state.tour) { state.drone = false; state.tourAngle = Math.atan2(state.z, state.x); } syncButtons(); }
function toggleDrone() {
  state.drone = !state.drone;
  if (!state.drone) { const p = { x: state.x, z: state.z }; collide(p, p); state.x = p.x; state.z = p.z; }
  state.pitch = state.drone ? -0.35 : 0.07;
  syncButtons();
}
function toggleFullscreen() {
  if (document.fullscreenElement) document.exitFullscreen();
  else document.documentElement.requestFullscreen?.();
}

// ── UI ──────────────────────────────────────────────────────────────────────
function setQuality(q) { state.quality = q; $('quality').value = q; accumN = 0; syncButtons(); }
function syncButtons() {
  $('tour').classList.toggle('on', state.tour);
  $('drone').classList.toggle('on', state.drone);
  $('photo').classList.toggle('on', state.quality === 'photo');
}
function applyPreset(name) {
  const p = PRESETS[name];
  state.sunElev = p.elev * DEG; state.sunAz = p.az;
  $('sun').value = p.elev; $('az').value = Math.round(p.az / DEG);
  document.querySelectorAll('[data-preset]').forEach((b) => b.classList.toggle('on', b.dataset.preset === name));
}

function wireUI() {
  $('sun').oninput = (e) => { state.sunElev = +e.target.value * DEG; document.querySelectorAll('[data-preset]').forEach((b) => b.classList.remove('on')); };
  $('az').oninput = (e) => { state.sunAz = +e.target.value * DEG; };
  $('exposure').oninput = (e) => { state.exposure = +e.target.value; };
  $('res').oninput = (e) => { state.scale = +e.target.value / 100; state.autoRes = false; $('autores').checked = false; };
  $('autores').onchange = (e) => { state.autoRes = e.target.checked; };
  $('quality').onchange = (e) => setQuality(e.target.value);
  $('shadows').onchange = (e) => { state.shadows = e.target.checked; };
  $('bloom').onchange = (e) => { state.bloom = e.target.checked; };
  $('denoise').onchange = (e) => { state.denoise = e.target.checked; };
  $('pause').onchange = (e) => { state.paused = e.target.checked; };
  $('tour').onclick = toggleTour;
  $('drone').onclick = toggleDrone;
  $('photo').onclick = () => setQuality(state.quality === 'photo' ? 'balanced' : 'photo');
  $('shot').onclick = screenshot;
  $('fullscreen').onclick = toggleFullscreen;
  $('panel-toggle').onclick = () => document.body.classList.toggle('panel-collapsed');
  document.querySelectorAll('[data-preset]').forEach((b) => (b.onclick = () => applyPreset(b.dataset.preset)));
  $('threads').textContent = threads;
  $('sun').value = state.sunElev / DEG; $('az').value = Math.round(state.sunAz / DEG);
  syncButtons();
  showValues();
}

function screenshot() {
  canvas.toBlob((blob) => {
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = `island-villa-${Date.now()}.png`;
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 2000);
  });
}

function showValues() {
  $('sun-val').textContent = `${(state.sunElev / DEG).toFixed(1)}°`;
  $('az-val').textContent = `${Math.round(((state.sunAz / DEG) % 360 + 360) % 360)}°`;
  $('exp-val').textContent = state.exposure.toFixed(2);
}

let statT = 0;
function updateStats(frameMs) {
  const now = performance.now();
  if (now - statT < 250) return;
  statT = now;
  showValues();
  $('fps').textContent = fps.toFixed(0);
  $('resout').textContent = `${rw}×${rh}`;
  $('ms').textContent = workerMs.toFixed(0);
  $('mpix').textContent = (rw * rh * fps / 1e6).toFixed(1);
  $('spp').textContent = accumN > 1 && (state.paused || state.quality === 'photo') ? accumN : (accumN > 1 ? 'temporal' : '1');
}

function resize() {
  const dpr = Math.min(devicePixelRatio || 1, 2);
  canvas.width = Math.round(innerWidth * dpr);
  canvas.height = Math.round(innerHeight * dpr);
}
addEventListener('resize', resize);

function setLoading(text, p) { $('load-text').textContent = text; $('load-bar').style.width = `${Math.round(p * 100)}%`; }
function fail(msg) { $('load-text').textContent = msg; $('loading').classList.add('error'); }

let lastT = 0;
function loop(t) {
  const dt = Math.min(0.1, (t - (lastT || t)) / 1000);
  lastT = t;
  update(dt);
  if (!busy) dispatch();
  requestAnimationFrame(loop);
}

function start() {
  $('loading').classList.add('gone');
  requestAnimationFrame(loop);
}

resize();
wireUI();
boot();
