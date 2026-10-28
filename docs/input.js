// input.js — keyboard, mouse and touch input, per-frame movement, and the tour / drone toggles.
import { $, canvas, state, EYE } from './state.js';
import { collide } from './collision.js';
import { setQuality, syncButtons } from './ui.js';

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

// ── movement ────────────────────────────────────────────────────────────────
export function update(dt) {
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
    collide(p, prev, state.drone);
    state.x = p.x; state.z = p.z;
  }
  const targetEye = state.drone ? 11 : EYE;
  state.eye += (targetEye - state.eye) * Math.min(1, dt * 3);
  if (Math.abs(targetEye - state.eye) < 0.002) state.eye = targetEye;
}

export function toggleTour() { state.tour = !state.tour; if (state.tour) { state.drone = false; state.tourAngle = Math.atan2(state.z, state.x); } syncButtons(); }
export function toggleDrone() {
  state.drone = !state.drone;
  if (!state.drone) { const p = { x: state.x, z: state.z }; collide(p, p, false); state.x = p.x; state.z = p.z; }
  state.pitch = state.drone ? -0.35 : 0.07;
  syncButtons();
}
export function toggleFullscreen() {
  if (document.fullscreenElement) document.exitFullscreen();
  else document.documentElement.requestFullscreen?.();
}
