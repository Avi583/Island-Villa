// sky.h — sky gradient, sun halo, stars, mountains, aerial haze and cheap ambient light.
#pragma once
#include "scene.h"

using namespace rt;

#include "frame.h"

// ── sky: gradient + Mie halo + sun disc + stars + procedural mountain range ──
static V3 skyColor(V3 d, bool sunDisk) {
  float y = d.y;
  float mu = dot(d, g_f.sunDir);
  float toward = sat(mu * 0.5f + 0.5f);
  V3 hor = mix(g_f.horizonAway, g_f.horizon, toward * toward);
  float t = fsqrt(fmaxx(y, 0.f));
  V3 col = t < 0.32f ? mix(hor, g_f.midSky, t / 0.32f)
                     : mix(g_f.midSky, g_f.zenith, sat((t - 0.32f) / 0.68f));
  float m = fmaxx(mu, 0.f);
  float m2 = m * m, m4 = m2 * m2, m8 = m4 * m4, m16 = m8 * m8;      // m^6 and m^80 by squaring (no exp2/log2)
  float m6 = m4 * m2, m80 = m16 * m16 * m16 * m16 * m16;
  col += g_f.sunCol * (0.05f * m6 + 0.22f * m80);
  if (sunDisk && mu > 0.99995f) col += g_f.sunCol * 45.f;

  if (sunDisk && g_f.night > 0.01f && y > 0.f) {     // stars: camera & mirror rays only, never GI
    V3 q = d * 220.f;
    u32 hs = hash32((u32)(i32)ffloor(q.x) * 73856093u ^ (u32)(i32)ffloor(q.y) * 19349663u ^ (u32)(i32)ffloor(q.z) * 83492791u);
    float hv = (float)(hs & 0xFFFFu) * (1.f / 65535.f);
    if (hv > 0.9982f) col += V3(1.f, 0.95f, 0.88f) * ((hv - 0.9982f) * 1500.f * g_f.night * sat(y * 6.f));
  }

  float az = fatan2(d.z, d.x);
  float mh = 0.032f + 0.020f * fsin(az * 3.f + 1.3f) + 0.011f * fsin(az * 7.f + 0.4f)
           + 0.005f * fsin(az * 17.f + 2.1f) + 0.0025f * fsin(az * 43.f);
  if (y < mh && y > -0.05f) {
    V3 rock = mix(hor * 0.6f, V3(0.05f, 0.06f, 0.08f) * (0.25f + 0.75f * g_f.day), 0.45f);
    col = mix(col, rock, smoothstep(mh, mh - 0.003f, y));
  }
  if (y < -0.05f) col = hor * 0.5f;
  return col;
}

static V3 hazeColor(V3 d) { return skyColor(normalize(V3(d.x, 0.12f, d.z)), false) * 0.95f; }

static V3 ambientCheap(V3 n) {
  return g_f.zenith * (1.4f * (0.5f + 0.5f * n.y)) + g_f.horizon * 0.22f + V3(0.012f, 0.012f, 0.016f);
}
