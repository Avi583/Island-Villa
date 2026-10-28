// denoise.js — CPU bilateral denoise, used only when WebGL is unavailable.

// Edge-aware (bilateral) denoise on the 8-bit display image. Each pixel becomes a weighted
// average of its 5×5 neighbourhood; the weight falls off with spatial distance and, more
// steeply, with colour difference, so noise averages out while real edges stay sharp.
// Only the display copy is filtered — the float accumulator in WASM is untouched.
// Runs on the GPU (see GPU_DENOISE_FS); this JS version is the fallback without WebGL.
export const DEN_BASE_SIGMA = 28;                                // colour tolerance in 8-bit units
const DEN_SPATIAL = new Float32Array(25);
for (let dy = -2; dy <= 2; dy++)
  for (let dx = -2; dx <= 2; dx++) DEN_SPATIAL[(dy + 2) * 5 + dx + 2] = Math.exp(-(dx * dx + dy * dy) / (2 * 1.5 * 1.5));
const denLut = new Float32Array(2048);                    // exp(-d²/2σ²), indexed by d² >> 5
let denLutSigma = -1;

export function denoise(src, dst, w, h, sigma) {
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
