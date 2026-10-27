// main.js — input, camera, collision and the frame pipeline that drives the
// C++/WebAssembly ray tracer across a pool of Web Workers. No rendering logic
// lives here: every pixel is produced by the C++ code in ../src.
'use strict';

const $ = (id) => document.getElementById(id);
const canvas = $('view');
let ctx = null;                                          // 2D context, only used when WebGL is unavailable
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
  quality: 'balanced', bloom: true, denoise: true, upscale: true, shadows: true,
  scale: 0.45, autoRes: true,
};

const QUALITY = {
  performance: { target: 24, giStill: 0.0, giMove: 0.0, maxW: 960 },
  balanced:    { target: 36, giStill: 0.7, giMove: 0.0, maxW: 1280 },   // GI ray costs the same at any weight, so none while moving
  photo:       { target: 1e9, giStill: 1.0, giMove: 1.0, maxW: 1920 },
};

const PRESETS = {
  golden: { elev: 7,   az: 2.3 },
  noon:   { elev: 58,  az: 1.9 },
  blue:   { elev: -4,  az: 2.6 },
  night:  { elev: -14, az: 2.6 },
};

// ── worker pool ─────────────────────────────────────────────────────────────
const threads = Math.max(2, Math.min(16, (navigator.hardwareConcurrency || 4) - 1));   // leave a core for the main thread
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
// Two frame buffers: workers fill one while the other waits for the next vsync to be shown.
// A new frame is dispatched the moment the previous one completes, so the workers never sit
// idle while the main thread denoises and presents, or while it waits for requestAnimationFrame.
let bufs = [], filling = null, latest = null, rw = 0, rh = 0;
let dimg = null, dimg32 = null;                           // JS denoise output (no-WebGL fallback)
let busy = false, pending = 0, frameNo = 0, dispatchT = 0, workerMs = 0, doneMs = 0;
let accumN = 0, lastKey = '';
const RES_STEP = 16;                                      // render width granularity (px)
const RES_COOLDOWN = 8;                                   // frames between auto-resolution changes
const MAX_SPP = 512;                                      // stop refining a frozen, still image after this many samples
let adaptWait = 0;
let fps = 0, lastDone = performance.now();

function makeBuffer(w, h) {
  const img = new ImageData(w, h);
  return { w, h, img, img32: new Uint32Array(img.data.buffer), accumN: 0, frozen: false };
}

function renderSize() {
  const q = QUALITY[state.quality];
  const scale = state.quality === 'photo' ? 1 : state.scale;
  let w = Math.max(160, Math.round(canvas.width * scale / RES_STEP) * RES_STEP);   // snapped so small scale drifts don't reallocate
  w = Math.min(w, q.maxW);
  const h = Math.max(90, Math.round(w * canvas.height / canvas.width));
  return [w, h];
}

function dispatch() {
  const [w, h] = renderSize();
  const cam = [state.x, state.eye, state.z, state.yaw, state.pitch];
  const timeFrozen = state.paused || state.quality === 'photo';
  const key = cam.map((v) => v.toFixed(4)).join() + `|${w}x${h}|${state.sunElev}|${state.sunAz}|${state.exposure}|${state.shadows}|${state.quality}` +
              (timeFrozen ? `|${state.time.toFixed(3)}` : '');
  if (timeFrozen && key === lastKey && accumN >= MAX_SPP) return;   // converged: idle until the camera or settings change
  if (w !== rw || h !== rh) {
    rw = w; rh = h;
    bufs = [makeBuffer(w, h), makeBuffer(w, h)];          // `latest` keeps its old buffer until shown
    filling = bufs[0];
    accumN = 0;
  }
  const still = key === lastKey;
  lastKey = key;
  accumN = still ? accumN + 1 : 1;

  // Progressive refinement: a true running average while the scene is frozen,
  // a short temporal window (EMA) while cars and waves are moving.
  let blend = 1 / accumN;
  if (!timeFrozen) blend = Math.max(blend, 0.1);
  // GI fades in over the first few still frames instead of popping on when the camera stops.
  const q = QUALITY[state.quality];
  const gi = still ? q.giMove + (q.giStill - q.giMove) * Math.min(1, (accumN - 1) / 4) : q.giMove;

  filling.accumN = accumN;
  filling.frozen = timeFrozen;
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
      filling.img32.set(src.subarray(i * m.w, (i + 1) * m.w), y * m.w);
    }
  }
  workerMs = Math.max(workerMs, m.ms);
  if (--pending === 0) frameDone();
}

function frameDone() {
  const now = performance.now();
  fps = fps * 0.85 + (1000 / Math.max(1, now - lastDone)) * 0.15;
  lastDone = now;
  doneMs = workerMs;                                      // workerMs restarts with the next dispatch
  latest = filling;                                       // newest finished frame; an unshown older one is dropped
  filling = filling === bufs[0] ? bufs[1] : bufs[0];
  adaptResolution(now - dispatchT, latest.accumN);
  busy = false;
  if (!document.hidden) dispatch();                       // hidden tab: the rAF loop restarts rendering
}

// Edge-aware (bilateral) denoise on the 8-bit display image. Each pixel becomes a weighted
// average of its 5×5 neighbourhood; the weight falls off with spatial distance and, more
// steeply, with colour difference, so noise averages out while real edges stay sharp.
// Only the display copy is filtered — the float accumulator in WASM is untouched.
// Runs on the GPU (see GPU_DENOISE_FS); this JS version is the fallback without WebGL.
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

// GPU present path. The frame is uploaded straight from its pixel buffer, optionally
// denoised at render resolution into a framebuffer texture, then upscaled to the canvas.
// Upscale: each output pixel is filtered along the local edge direction (so edges stay clean
// instead of stair-stepping or smearing), then sharpened with an unsharp mask clamped to the
// neighbourhood's min/max (so it adds crispness without ringing).
const GPU_VS = `attribute vec2 p; varying vec2 uv;
  void main() { uv = p * 0.5 + 0.5; gl_Position = vec4(p, 0.0, 1.0); }`;
const GPU_VS_FLIP = `attribute vec2 p; varying vec2 uv;
  void main() { uv = vec2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5); gl_Position = vec4(p, 0.0, 1.0); }`;
const GPU_DENOISE_FS = `precision highp float;
  varying vec2 uv; uniform sampler2D tex; uniform vec2 texel; uniform float k;
  void main() {
    vec3 c = texture2D(tex, uv).rgb, sum = vec3(0.0);
    float sw = 0.0;
    for (int dy = -2; dy <= 2; dy++)
      for (int dx = -2; dx <= 2; dx++) {
        vec2 o = vec2(float(dx), float(dy));
        vec3 q = texture2D(tex, uv + o * texel).rgb, d = q - c;
        float w = exp(-dot(o, o) / 4.5 - dot(d, d) * k);   // spatial σ = 1.5 px, k = 255² / 2σ²
        sum += q * w; sw += w;
      }
    gl_FragColor = vec4(sum / sw, 1.0);
  }`;
const GPU_UPSCALE_FS = `precision highp float;
  varying vec2 uv; uniform sampler2D tex, bloom; uniform vec2 texel; uniform float amount, bloomAmt;
  float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
  vec3 s(vec2 p) { return texture2D(tex, p).rgb; }
  // Glow: the blurred 1/4-size copy, pushed through brightness 1.15 / contrast 1.6, added on top.
  vec3 glow() {
    if (bloomAmt <= 0.0) return vec3(0.0);
    vec3 b = texture2D(bloom, uv).rgb;
    return bloomAmt * clamp((b * 1.15 - 0.5) * 1.6 + 0.5, 0.0, 1.0);
  }
  void main() {
    vec3 c = s(uv);
    if (amount <= 0.0) { gl_FragColor = vec4(min(c + glow(), 1.0), 1.0); return; }   // plain bilinear
    vec3 n = s(uv + vec2(0.0, -texel.y)), so = s(uv + vec2(0.0, texel.y));
    vec3 w = s(uv + vec2(-texel.x, 0.0)), e = s(uv + vec2(texel.x, 0.0));
    // luma gradient -> edge tangent; filter along it only
    vec2 g = vec2(luma(e) - luma(w), luma(so) - luma(n));
    float gl2 = dot(g, g);
    vec3 base = c;
    if (gl2 > 1e-5) {
      vec2 t = vec2(-g.y, g.x) * inversesqrt(gl2) * texel * 0.7;
      base = (c * 2.0 + s(uv + t) + s(uv - t)) * 0.25;
    }
    // contrast-clamped unsharp mask
    vec3 blur = (n + so + w + e) * 0.25;
    vec3 lo = min(min(n, so), min(min(w, e), c)), hi = max(max(n, so), max(max(w, e), c));
    vec3 outc = clamp(base + (base - blur) * amount, lo, hi);
    gl_FragColor = vec4(min(outc + glow(), 1.0), 1.0);
  }`;
// One axis of a 9-tap gaussian (σ = 2 taps); `step` is the tap spacing in uv units.
const GPU_BLUR_FS = `precision highp float;
  varying vec2 uv; uniform sampler2D tex; uniform vec2 step;
  void main() {
    vec3 sum = vec3(0.0);
    float sw = 0.0;
    for (int i = -4; i <= 4; i++) {
      float w = exp(-float(i * i) / 8.0);
      sum += texture2D(tex, uv + step * float(i)).rgb * w; sw += w;
    }
    gl_FragColor = vec4(sum / sw, 1.0);
  }`;
const gpu = { gl: null, failed: false, canvas: null, srcTex: null, fbo: null, fboTex: null, fw: 0, fh: 0, den: null, up: null,
  blur: null, bloomTex: [null, null], bloomFbo: [null, null], bw: 0, bh: 0 };

function initGPU() {
  gpu.canvas = canvas;                                    // render straight into the visible canvas
  // preserveDrawingBuffer keeps the last frame readable for the Screenshot button.
  const gl = canvas.getContext('webgl', { alpha: false, antialias: false, premultipliedAlpha: false, preserveDrawingBuffer: true });
  if (!gl) return false;
  const sh = (type, src) => {
    const o = gl.createShader(type); gl.shaderSource(o, src); gl.compileShader(o);
    return gl.getShaderParameter(o, gl.COMPILE_STATUS) ? o : null;
  };
  const program = (vsSrc, fsSrc, uniforms) => {
    const v = sh(gl.VERTEX_SHADER, vsSrc), f = sh(gl.FRAGMENT_SHADER, fsSrc);
    if (!v || !f) return null;
    const prog = gl.createProgram();
    gl.attachShader(prog, v); gl.attachShader(prog, f);
    gl.bindAttribLocation(prog, 0, 'p');                  // both programs share one quad
    gl.linkProgram(prog);
    if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) return null;
    const loc = { prog };
    for (const u of uniforms) loc[u] = gl.getUniformLocation(prog, u);
    return loc;
  };
  gpu.den = program(GPU_VS, GPU_DENOISE_FS, ['texel', 'k']);
  gpu.up = program(GPU_VS_FLIP, GPU_UPSCALE_FS, ['tex', 'bloom', 'texel', 'amount', 'bloomAmt']);
  gpu.blur = program(GPU_VS, GPU_BLUR_FS, ['step']);
  if (!gpu.den || !gpu.up || !gpu.blur) return false;
  canvas.addEventListener('webglcontextlost', (e) => e.preventDefault());
  canvas.addEventListener('webglcontextrestored', () => { gpu.gl = null; gpu.fw = gpu.fh = gpu.bw = gpu.bh = 0; });
  gl.bindBuffer(gl.ARRAY_BUFFER, gl.createBuffer());
  gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
  gl.enableVertexAttribArray(0);
  gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
  const texture = () => {
    const t = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, t);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    return t;
  };
  gpu.srcTex = texture();
  gpu.fboTex = texture();
  gpu.fbo = gl.createFramebuffer();
  for (let i = 0; i < 2; i++) { gpu.bloomTex[i] = texture(); gpu.bloomFbo[i] = gl.createFramebuffer(); }
  gpu.gl = gl;
  return true;
}

// Draws frame `f` to the main canvas on the GPU (denoised when sigma > 0, sharpened when
// amount > 0). Returns false without WebGL so the caller can use the 2D fallback.
function drawGPU(f, sigma, amount, bloom) {
  if (gpu.failed) return false;
  if (!gpu.gl && !initGPU()) { gpu.failed = true; return false; }
  const gl = gpu.gl;
  if (gl.isContextLost()) return true;                    // skip the frame; webglcontextrestored re-inits
  let tex = gpu.srcTex;
  if (sigma > 0) {
    if (gpu.fw !== f.w || gpu.fh !== f.h) {
      gl.bindTexture(gl.TEXTURE_2D, gpu.fboTex);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, f.w, f.h, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
      gl.bindFramebuffer(gl.FRAMEBUFFER, gpu.fbo);
      gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, gpu.fboTex, 0);
      if (gl.checkFramebufferStatus(gl.FRAMEBUFFER) !== gl.FRAMEBUFFER_COMPLETE) { gpu.failed = true; return false; }
      gpu.fw = f.w; gpu.fh = f.h;
    }
    tex = gpu.fboTex;
  }
  gl.bindTexture(gl.TEXTURE_2D, gpu.srcTex);
  gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, f.img);
  if (sigma > 0) {
    gl.bindFramebuffer(gl.FRAMEBUFFER, gpu.fbo);
    gl.viewport(0, 0, f.w, f.h);
    gl.useProgram(gpu.den.prog);
    gl.uniform2f(gpu.den.texel, 1 / f.w, 1 / f.h);
    gl.uniform1f(gpu.den.k, 65025 / (2 * sigma * sigma));
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
  }
  if (bloom) {
    // Blur a 1/4-size copy (horizontal then vertical); the upscale pass adds it back.
    const bw = Math.max(1, canvas.width >> 2), bh = Math.max(1, canvas.height >> 2);
    if (gpu.bw !== bw || gpu.bh !== bh) {
      for (let i = 0; i < 2; i++) {
        gl.bindTexture(gl.TEXTURE_2D, gpu.bloomTex[i]);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, bw, bh, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
        gl.bindFramebuffer(gl.FRAMEBUFFER, gpu.bloomFbo[i]);
        gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, gpu.bloomTex[i], 0);
      }
      gpu.bw = bw; gpu.bh = bh;
    }
    gl.viewport(0, 0, bw, bh);
    gl.useProgram(gpu.blur.prog);
    const sigma = 1 / 90;                                  // blur σ as a fraction of canvas width (matches the old CSS blur)
    gl.bindFramebuffer(gl.FRAMEBUFFER, gpu.bloomFbo[0]);
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.uniform2f(gpu.blur.step, sigma / 2, 0);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    gl.bindFramebuffer(gl.FRAMEBUFFER, gpu.bloomFbo[1]);
    gl.bindTexture(gl.TEXTURE_2D, gpu.bloomTex[0]);
    gl.uniform2f(gpu.blur.step, 0, sigma / 2 * canvas.width / canvas.height);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.activeTexture(gl.TEXTURE1);
    gl.bindTexture(gl.TEXTURE_2D, gpu.bloomTex[1]);
    gl.activeTexture(gl.TEXTURE0);
  }
  gl.viewport(0, 0, canvas.width, canvas.height);
  gl.useProgram(gpu.up.prog);
  gl.bindTexture(gl.TEXTURE_2D, tex);
  gl.uniform1i(gpu.up.tex, 0);
  gl.uniform1i(gpu.up.bloom, 1);
  gl.uniform1f(gpu.up.bloomAmt, bloom ? 0.16 : 0);
  gl.uniform2f(gpu.up.texel, 1 / f.w, 1 / f.h);
  gl.uniform1f(gpu.up.amount, amount);
  gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
  return true;
}

function present(f) {
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
  state.scale = Math.min(1, Math.max(0.18, state.scale));
  if (state.scale !== before) adaptWait = RES_COOLDOWN;
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

const POOL = { x0: -7.9, x1: 7.9, z0: 8.5, z1: 12.1 };      // water edge + body radius

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
  if (p.x > POOL.x0 && p.x < POOL.x1 && p.z > POOL.z0 && p.z < POOL.z1) {            // don't walk into the pool
    const pen = [p.x - POOL.x0, POOL.x1 - p.x, p.z - POOL.z0, POOL.z1 - p.z];
    const i = pen.indexOf(Math.min(...pen));
    if (i === 0) p.x = POOL.x0; else if (i === 1) p.x = POOL.x1; else if (i === 2) p.z = POOL.z0; else p.z = POOL.z1;
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
}

let statT = 0;
function updateStats() {
  const now = performance.now();
  if (now - statT < 250) return;
  statT = now;
  showValues();
  $('fps').textContent = fps.toFixed(0);
  $('resout').textContent = `${rw}×${rh}`;
  $('ms').textContent = doneMs.toFixed(0);
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
  if (!busy && !document.hidden) dispatch();             // kick-start; frameDone() keeps workers busy after that
  if (latest) { present(latest); latest = null; }
  requestAnimationFrame(loop);
}

function start() {
  $('loading').classList.add('gone');
  requestAnimationFrame(loop);
}

resize();
wireUI();
boot();
