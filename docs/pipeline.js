// pipeline.js — worker pool and frame pipeline: boot, dispatch, row collection, frame
// completion, auto-resolution and presenting a finished frame (GPU, or 2D fallback).
import { $, canvas, state, QUALITY, threads, pipe } from './state.js';
import { drawGPU } from './gpu.js';
import { denoise, DEN_BASE_SIGMA } from './denoise.js';
import { updateStats, setLoading, fail } from './ui.js';

let ctx = null;                                          // 2D context, only used when WebGL is unavailable
const low = document.createElement('canvas');            // internal render target
const lctx = low.getContext('2d', { alpha: false });

// ── worker pool ─────────────────────────────────────────────────────────────
const workers = [];
let ready = 0;
let whenReady = null;

export async function boot(onReady) {
  whenReady = onReady;
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
    if (ready === threads) whenReady();
  } else if (m.type === 'rows') {
    receiveRows(m);
  } else if (m.type === 'error') {
    fail(m.message);
  }
}

// ── frame pipeline ──────────────────────────────────────────────────────────
// Two frame buffers: workers fill one while the other waits for the next vsync to be shown.
// A new frame is dispatched the moment the previous one completes, so the workers never sit
// idle while the main thread denoises and presents, or while it waits for requestAnimationFrame.
// rw, rh, latest, busy, accumN, fps and doneMs live in `pipe` (state.js).
let bufs = [], filling = null;
let dimg = null, dimg32 = null;                           // JS denoise output (no-WebGL fallback)
let pending = 0, frameNo = 0, dispatchT = 0, workerMs = 0;
let lastKey = '';
const RES_STEP = 16;                                      // render width granularity (px)
const RES_COOLDOWN = 8;                                   // frames between auto-resolution changes
const MIN_RENDER_W = 320;                                 // never render narrower than this (px)
const MAX_SPP = 512;                                      // stop refining a frozen, still image after this many samples
let adaptWait = 0;
let lastDone = performance.now();


function makeBuffer(w, h) {
  const img = new ImageData(w, h);
  return { w, h, img, img32: new Uint32Array(img.data.buffer), depth: new Float32Array(w * h), cam: null, fov: 0,
           accumN: 0, frozen: false };
}

function renderSize() {
  const q = QUALITY[state.quality];
  const scale = state.quality === 'photo' ? 1 : state.scale;
  let w = Math.max(MIN_RENDER_W, Math.round(canvas.width * scale / RES_STEP) * RES_STEP);   // snapped so small scale drifts don't reallocate
  w = Math.min(w, q.maxW);
  const h = Math.max(180, Math.round(w * canvas.height / canvas.width));
  return [w, h];
}

export function dispatch() {
  const [w, h] = renderSize();
  const cam = [state.x, state.eye, state.z, state.yaw, state.pitch];
  const timeFrozen = state.paused || state.quality === 'photo';
  const key = cam.map((v) => v.toFixed(4)).join() + `|${w}x${h}|${state.sunElev}|${state.sunAz}|${state.exposure}|${state.shadows}|${state.quality}` +
              (timeFrozen ? `|${state.time.toFixed(3)}` : '');
  if (timeFrozen && key === lastKey && pipe.accumN >= MAX_SPP) return;   // converged: idle until the camera or settings change
  if (w !== pipe.rw || h !== pipe.rh) {
    pipe.rw = w; pipe.rh = h;
    bufs = [makeBuffer(w, h), makeBuffer(w, h)];          // `latest` keeps its old buffer until shown
    filling = bufs[0];
    pipe.accumN = 0;
  }
  const still = key === lastKey;
  lastKey = key;
  pipe.accumN = still ? pipe.accumN + 1 : 1;

  // Progressive refinement: a true running average while the scene is frozen,
  // a short temporal window (EMA) while cars and waves are moving.
  let blend = 1 / pipe.accumN;
  if (!timeFrozen) blend = Math.max(blend, 0.1);
  // GI fades in over the first few still frames instead of popping on when the camera stops.
  const q = QUALITY[state.quality];
  const gi = still ? q.giMove + (q.giStill - q.giMove) * Math.min(1, (pipe.accumN - 1) / 4) : q.giMove;

  filling.accumN = pipe.accumN;
  filling.frozen = timeFrozen;
  filling.cam = cam;
  filling.fov = state.fov;
  pipe.busy = true;
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
  if (m.w === pipe.rw && m.h === pipe.rh) {
    const src = new Uint32Array(m.buf), dsrc = new Float32Array(m.depth);
    for (let i = 0; i < m.rows; i++) {
      const y = m.index + i * threads;
      filling.img32.set(src.subarray(i * m.w, (i + 1) * m.w), y * m.w);
      filling.depth.set(dsrc.subarray(i * m.w, (i + 1) * m.w), y * m.w);
    }
  }
  workerMs = Math.max(workerMs, m.ms);
  if (--pending === 0) frameDone();
}

function frameDone() {
  const now = performance.now();
  pipe.fps = pipe.fps * 0.85 + (1000 / Math.max(1, now - lastDone)) * 0.15;
  lastDone = now;
  pipe.doneMs = workerMs;                                      // workerMs restarts with the next dispatch
  pipe.latest = filling;                                       // newest finished frame; an unshown older one is dropped
  filling = filling === bufs[0] ? bufs[1] : bufs[0];
  adaptResolution(now - dispatchT, pipe.latest.accumN);
  pipe.busy = false;
  if (!document.hidden) dispatch();                       // hidden tab: the rAF loop restarts rendering
}

export function present(f) {
  // Strength fades as a frozen/still image converges; while time runs the temporal window stays short, so keep it full.
  const strength = f.frozen ? 1 / Math.sqrt(Math.max(1, f.accumN)) : 1;
  const sigma = state.denoise && strength > 0.15 ? DEN_BASE_SIGMA * strength : 0;
  // Sharpen harder the more the frame is stretched; skip when nearly 1:1 or once the image has converged.
  const stretch = canvas.width / f.w;
  const settled = f.frozen && f.accumN > 8;
  const amount = state.upscale && stretch > 1.08 && !settled ? Math.min(1.2, 0.35 * (stretch - 1) + 0.25) : 0;
  if (!drawGPU(f, sigma, amount, state.bloom)) {                       // no WebGL: denoise in JS, let the 2D canvas scale
    if (!ctx && !(ctx = canvas.getContext('2d', { alpha: false }))) return;
    ctx.globalCompositeOperation = 'source-over';
    ctx.globalAlpha = 1;
    ctx.filter = 'none';
    let out = f.img;
    if (sigma > 0) {
      if (!dimg || dimg.width !== f.w || dimg.height !== f.h) { dimg = new ImageData(f.w, f.h); dimg32 = new Uint32Array(dimg.data.buffer); }
      denoise(f.img32, dimg32, f.w, f.h, sigma);
      out = dimg;
    }
    if (low.width !== f.w || low.height !== f.h) { low.width = f.w; low.height = f.h; }
    lctx.putImageData(out, 0, 0);
    ctx.imageSmoothingEnabled = true;
    ctx.imageSmoothingQuality = 'high';
    ctx.drawImage(low, 0, 0, canvas.width, canvas.height);
    if (state.bloom) {                                    // cheap glow pass for neon and sun
      ctx.globalCompositeOperation = 'lighter';
      ctx.globalAlpha = 0.16;
      ctx.filter = `blur(${Math.round(canvas.width / 90)}px) brightness(1.15) contrast(1.6)`;
      ctx.drawImage(low, 0, 0, canvas.width, canvas.height);
      ctx.filter = 'none';
      ctx.globalAlpha = 1;
      ctx.globalCompositeOperation = 'source-over';
    }
  }
  updateStats();
}

function adaptResolution(frameMs, n) {
  if (!state.autoRes || state.quality === 'photo' || n > 1) return;   // never resize mid-accumulation
  const target = QUALITY[state.quality].target;
  if (adaptWait > 0) { adaptWait--; return; }
  const before = state.scale;
  if (frameMs > target * 1.25) state.scale *= 0.92;
  else if (frameMs < target * 0.7) state.scale *= 1.04;
  state.scale = Math.min(1, Math.max(state.minScale, state.scale));
  if (state.scale !== before) adaptWait = RES_COOLDOWN;
  $('res').value = Math.round(state.scale * 100);
}
