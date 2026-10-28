// gpu.js — WebGL present path: temporal reprojection, denoise, bloom and edge-aware upscale.
import { canvas } from './state.js';
import { GPU_VS, GPU_VS_FLIP, GPU_DENOISE_FS, GPU_UPSCALE_FS, GPU_TAA_FS, GPU_BLUR_FS } from './shaders.js';

// GPU present path. The frame is uploaded straight from its pixel buffer, optionally
// denoised at render resolution into a framebuffer texture, then upscaled to the canvas.
// Upscale: each output pixel is filtered along the local edge direction (so edges stay clean
// instead of stair-stepping or smearing), then sharpened with an unsharp mask clamped to the
// neighbourhood's min/max (so it adds crispness without ringing).
const TAA_ALPHA = 0.125;                                  // weight of the new frame while moving (~8-frame window)
const TAA_SETTLE = 8;                                     // once still, WASM accumulation takes over within this many frames
// One axis of a 9-tap gaussian (σ = 2 taps); `step` is the tap spacing in uv units.
const gpu = { gl: null, failed: false, canvas: null, srcTex: null, fbo: null, fboTex: null, fw: 0, fh: 0, den: null, up: null,
  blur: null, bloomTex: [null, null], bloomFbo: [null, null], bw: 0, bh: 0,
  taa: null, depthTex: null, histTex: [null, null], histFbo: [null, null], hw: 0, hh: 0, hi: 0, prevCam: null };

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
  // Temporal pass needs float textures for depth; without them frames are shown as rendered.
  gpu.taa = gl.getExtension('OES_texture_float') &&
            program(GPU_VS, GPU_TAA_FS, ['cur', 'hist', 'depth', 'texel', 'alpha', 'camP', 'camF', 'camR', 'camU', 'prevP', 'prevF', 'prevR', 'prevU']);
  canvas.addEventListener('webglcontextlost', (e) => e.preventDefault());
  canvas.addEventListener('webglcontextrestored', () => { gpu.gl = null; gpu.fw = gpu.fh = gpu.bw = gpu.bh = gpu.hw = gpu.hh = 0; gpu.prevCam = null; });
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
  for (let i = 0; i < 2; i++) { gpu.histTex[i] = texture(); gpu.histFbo[i] = gl.createFramebuffer(); }
  gpu.depthTex = texture();
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);   // float textures can't be filtered in WebGL1
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
  gpu.gl = gl;
  return true;
}

// Camera basis matching rt_render: forward, right and up, plus the image-plane half extents.
function camBasis(cam, fov, aspect) {
  const [x, y, z, yaw, pitch] = cam;
  const cp = Math.cos(pitch), f = [cp * Math.cos(yaw), Math.sin(pitch), cp * Math.sin(yaw)];
  const r = [-f[2] / cp, 0, f[0] / cp];                   // normalize(cross(f, +Y))
  const u = [r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]];
  const th = Math.tan(fov / 2);
  return { p: [x, y, z], f, r, u, sx: th * aspect, sy: th };
}

// Blends frame `f` (already uploaded to srcTex) with the reprojected history; returns the result texture.
function temporal(f) {
  const gl = gpu.gl, t = gpu.taa;
  if (gpu.hw !== f.w || gpu.hh !== f.h) {
    for (let i = 0; i < 2; i++) {
      gl.bindTexture(gl.TEXTURE_2D, gpu.histTex[i]);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, f.w, f.h, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
      gl.bindFramebuffer(gl.FRAMEBUFFER, gpu.histFbo[i]);
      gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, gpu.histTex[i], 0);
      if (gl.checkFramebufferStatus(gl.FRAMEBUFFER) !== gl.FRAMEBUFFER_COMPLETE) {
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
        gpu.taa = null;
        return gpu.srcTex;
      }
    }
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gpu.hw = f.w; gpu.hh = f.h; gpu.prevCam = null;
  }
  const cur = camBasis(f.cam, f.fov, f.w / f.h), prev = gpu.prevCam;
  gpu.prevCam = cur;
  const alpha = prev ? Math.min(1, Math.max(TAA_ALPHA, f.accumN / TAA_SETTLE)) : 1;
  const scaled = (v, k) => [v[0] * k, v[1] * k, v[2] * k];
  gl.activeTexture(gl.TEXTURE2);
  gl.bindTexture(gl.TEXTURE_2D, gpu.depthTex);
  gl.texImage2D(gl.TEXTURE_2D, 0, gl.LUMINANCE, f.w, f.h, 0, gl.LUMINANCE, gl.FLOAT, f.depth);
  gl.activeTexture(gl.TEXTURE1);
  gl.bindTexture(gl.TEXTURE_2D, gpu.histTex[gpu.hi]);    // last shown result
  gl.activeTexture(gl.TEXTURE0);
  gl.bindTexture(gl.TEXTURE_2D, gpu.srcTex);
  gpu.hi ^= 1;
  gl.bindFramebuffer(gl.FRAMEBUFFER, gpu.histFbo[gpu.hi]);
  gl.viewport(0, 0, f.w, f.h);
  gl.useProgram(t.prog);
  gl.uniform1i(t.cur, 0); gl.uniform1i(t.hist, 1); gl.uniform1i(t.depth, 2);
  gl.uniform2f(t.texel, 1 / f.w, 1 / f.h);
  gl.uniform1f(t.alpha, alpha);
  gl.uniform3fv(t.camP, cur.p); gl.uniform3fv(t.camF, cur.f);
  gl.uniform3fv(t.camR, scaled(cur.r, cur.sx)); gl.uniform3fv(t.camU, scaled(cur.u, cur.sy));
  const pv = prev || cur;
  gl.uniform3fv(t.prevP, pv.p); gl.uniform3fv(t.prevF, pv.f);
  gl.uniform3fv(t.prevR, scaled(pv.r, 1 / pv.sx)); gl.uniform3fv(t.prevU, scaled(pv.u, 1 / pv.sy));
  gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
  gl.bindFramebuffer(gl.FRAMEBUFFER, null);
  return gpu.histTex[gpu.hi];
}

// Draws frame `f` to the main canvas on the GPU (denoised when sigma > 0, sharpened when
// amount > 0). Returns false without WebGL so the caller can use the 2D fallback.
export function drawGPU(f, sigma, amount, bloom) {
  if (gpu.failed) return false;
  if (!gpu.gl && !initGPU()) { gpu.failed = true; return false; }
  const gl = gpu.gl;
  if (gl.isContextLost()) return true;                    // skip the frame; webglcontextrestored re-inits
  gl.bindTexture(gl.TEXTURE_2D, gpu.srcTex);
  gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, f.img);
  let tex = gpu.taa ? temporal(f) : gpu.srcTex;
  if (sigma > 0) {
    if (gpu.fw !== f.w || gpu.fh !== f.h) {
      gl.bindTexture(gl.TEXTURE_2D, gpu.fboTex);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, f.w, f.h, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
      gl.bindFramebuffer(gl.FRAMEBUFFER, gpu.fbo);
      gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, gpu.fboTex, 0);
      if (gl.checkFramebufferStatus(gl.FRAMEBUFFER) !== gl.FRAMEBUFFER_COMPLETE) { gpu.failed = true; return false; }
      gpu.fw = f.w; gpu.fh = f.h;
    }
    gl.bindFramebuffer(gl.FRAMEBUFFER, gpu.fbo);
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.viewport(0, 0, f.w, f.h);
    gl.useProgram(gpu.den.prog);
    gl.uniform2f(gpu.den.texel, 1 / f.w, 1 / f.h);
    gl.uniform1f(gpu.den.k, 65025 / (2 * sigma * sigma));
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    tex = gpu.fboTex;
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
