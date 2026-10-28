// tonemap.h — ACES tone mapping, gamma lookup table and 8-bit sRGB output.
#pragma once
#include "scene.h"

using namespace rt;

// ── tone mapping ─────────────────────────────────────────────────────────────
static float aces(float x) {           // Narkowicz ACES fit
  return sat((x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f));
}
static const int GAMMA_LUT_N = 4096;
static float g_gammaLut[GAMMA_LUT_N + 2];   // 255 * x^(1/2.2), x in [0,1]
static void buildGammaLut() {
  for (int i = 0; i <= GAMMA_LUT_N + 1; i++)
    g_gammaLut[i] = fpow((float)i / (float)GAMMA_LUT_N, 1.f / 2.2f) * 255.f;
}
static u32 toSRGB8(float x, float dither) {
  float f = aces(x) * (float)GAMMA_LUT_N;   // aces() is already in [0,1]
  int i = (int)f;
  float v = g_gammaLut[i] + (g_gammaLut[i + 1] - g_gammaLut[i]) * (f - (float)i) + dither;
  return (u32)clampf(v, 0.f, 255.f);
}
