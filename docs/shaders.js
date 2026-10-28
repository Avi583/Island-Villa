// shaders.js — GLSL sources for the WebGL present path (denoise, upscale + bloom, temporal reprojection, blur).
// See the GPU present path in main.js for how they are used.
export const GPU_VS = `attribute vec2 p; varying vec2 uv;
  void main() { uv = p * 0.5 + 0.5; gl_Position = vec4(p, 0.0, 1.0); }`;
export const GPU_VS_FLIP = `attribute vec2 p; varying vec2 uv;
  void main() { uv = vec2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5); gl_Position = vec4(p, 0.0, 1.0); }`;
export const GPU_DENOISE_FS = `precision highp float;
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
export const GPU_UPSCALE_FS = `precision highp float;
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
// Temporal reprojection: each pixel's world point (camera ray × reprojection depth from the
// C++ tracer, which looks through thin glass) is projected into the previously shown camera
// and the history there is blended in, so noise keeps averaging out while the camera moves.
// History is clamped to the current 3×3 neighbourhood: noisy areas (the interior behind the
// glass) span a wide range and keep their history, clean edges a narrow one, which stops ghosting.
export const GPU_TAA_FS = `precision highp float;
  varying vec2 uv; uniform sampler2D cur, hist, depth; uniform vec2 texel; uniform float alpha;
  uniform vec3 camP, camF, camR, camU, prevP, prevF, prevR, prevU;   // R/U scaled to the image plane
  float vig(vec2 q) { return 1.0 - 0.22 * (q.x * q.x * 0.6 + q.y * q.y); }   // same as rt_render
  void main() {
    vec3 c = texture2D(cur, uv).rgb;
    vec2 q = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    vec3 d = camP + normalize(camF + camR * q.x + camU * q.y) * texture2D(depth, uv).r - prevP;
    float z = dot(d, prevF);
    vec2 pq = vec2(dot(d, prevR), dot(d, prevU)) / max(z, 1e-4);
    vec2 puv = vec2(pq.x * 0.5 + 0.5, 0.5 - pq.y * 0.5);
    if (alpha >= 1.0 || z <= 0.0 || puv != clamp(puv, 0.0, 1.0)) { gl_FragColor = vec4(c, 1.0); return; }
    vec3 h = texture2D(hist, puv).rgb * pow(vig(q) / vig(pq), 1.0 / 2.2);   // vignette is baked into both frames
    vec3 lo = c, hi = c;
    for (int dy = -1; dy <= 1; dy++)
      for (int dx = -1; dx <= 1; dx++) {
        vec3 n = texture2D(cur, uv + vec2(float(dx), float(dy)) * texel).rgb;
        lo = min(lo, n); hi = max(hi, n);
      }
    gl_FragColor = vec4(mix(clamp(h, lo, hi), c, alpha), 1.0);
  }`;
export const GPU_BLUR_FS = `precision highp float;
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
