// water.h — directional sine-wave water normals.
#pragma once
#include "scene.h"

using namespace rt;

#include "frame.h"

// ── water: sum of directional sine waves with analytic derivatives ──────────
static const float WAVES[8][5] = {   // dir x, dir z, wavenumber, speed, amplitude
  { 0.80f,  0.60f, 0.35f, 0.9f, 0.100f}, {-0.60f,  0.80f, 0.55f, 1.1f, 0.060f},
  { 0.30f, -0.95f, 0.90f, 1.5f, 0.035f}, {-0.91f, -0.41f, 1.40f, 1.9f, 0.022f},
  { 0.95f,  0.31f, 2.30f, 2.6f, 0.012f}, {-0.20f,  0.98f, 3.70f, 3.3f, 0.007f},
  { 0.60f, -0.80f, 5.90f, 4.2f, 0.004f}, {-0.71f,  0.71f, 9.10f, 5.1f, 0.0025f},
};
static V3 waveNormal(float x, float z, float t, float amp) {
  float dx = 0.f, dz = 0.f;
  for (int i = 0; i < 8; ++i) {
    const float* w = WAVES[i];
    float ph = (w[0] * x + w[1] * z) * w[2] + t * w[3];
    float s = w[4] * w[2] * fcos(ph);
    dx += w[0] * s; dz += w[1] * s;
  }
  return normalize(V3(-dx * amp, 1.f, -dz * amp));
}
static V3 waterNormal(V3 p, i32 mat, V3 nf, float dist) {
  if (fabsx(nf.y) < 0.5f) return nf;                 // pool side walls stay flat
  V3 w = mat == M_POOL ? waveNormal(p.x * 2.2f, p.z * 2.2f, g_f.time * 1.3f, 0.3f)
                       : waveNormal(p.x, p.z, g_f.time, 1.f / (1.f + dist * 0.012f));
  return nf.y > 0.f ? w : -w;
}
