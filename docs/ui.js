// ui.js — settings panel wiring, sun presets, screenshot, stats readout, loading screen, resize.
import { $, canvas, state, PRESETS, DEG, threads, pipe } from './state.js';
import { toggleTour, toggleDrone, toggleFullscreen } from './input.js';

// ── UI ──────────────────────────────────────────────────────────────────────
export function setQuality(q) { state.quality = q; $('quality').value = q; pipe.accumN = 0; syncButtons(); }
export function syncButtons() {
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

export function wireUI() {
  $('sun').oninput = (e) => { state.sunElev = +e.target.value * DEG; document.querySelectorAll('[data-preset]').forEach((b) => b.classList.remove('on')); };
  $('az').oninput = (e) => { state.sunAz = +e.target.value * DEG; };
  $('exposure').oninput = (e) => { state.exposure = +e.target.value; };
  $('res').oninput = (e) => { state.scale = +e.target.value / 100; state.autoRes = false; $('autores').checked = false; };
  $('autores').onchange = (e) => { state.autoRes = e.target.checked; };
  $('minres').oninput = (e) => {
    state.minScale = +e.target.value / 100;
    if (state.scale < state.minScale) { state.scale = state.minScale; $('res').value = Math.round(state.scale * 100); }
    showValues();
  };
  $('quality').onchange = (e) => setQuality(e.target.value);
  $('shadows').onchange = (e) => { state.shadows = e.target.checked; };
  $('bloom').onchange = (e) => { state.bloom = e.target.checked; };
  $('denoise').onchange = (e) => { state.denoise = e.target.checked; };
  $('upscale').onchange = (e) => { state.upscale = e.target.checked; };
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
  $('minres-val').textContent = `${Math.round(state.minScale * 100)}%`;
}

let statT = 0;
export function updateStats() {
  const now = performance.now();
  if (now - statT < 250) return;
  statT = now;
  showValues();
  $('fps').textContent = pipe.fps.toFixed(0);
  $('resout').textContent = `${pipe.rw}×${pipe.rh}`;
  $('ms').textContent = pipe.doneMs.toFixed(0);
  $('mpix').textContent = (pipe.rw * pipe.rh * pipe.fps / 1e6).toFixed(1);
  $('spp').textContent = pipe.accumN > 1 && (state.paused || state.quality === 'photo') ? pipe.accumN : (pipe.accumN > 1 ? 'temporal' : '1');
}

export function resize() {
  const dpr = Math.min(devicePixelRatio || 1, 2);
  canvas.width = Math.round(innerWidth * dpr);
  canvas.height = Math.round(innerHeight * dpr);
}
addEventListener('resize', resize);

export function setLoading(text, p) { $('load-text').textContent = text; $('load-bar').style.width = `${Math.round(p * 100)}%`; }
export function fail(msg) { $('load-text').textContent = msg; $('loading').classList.add('error'); }
