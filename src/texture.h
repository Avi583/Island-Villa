// texture.h — value noise and procedural albedo textures.
#pragma once
#include "scene.h"

using namespace rt;

// ── procedural textures ──────────────────────────────────────────────────────
static float hash2(i32 x, i32 y) { return (float)hash32((u32)x * 73856093u ^ (u32)y * 19349663u) * (1.f / 4294967296.f); }
static float vnoise(float x, float y) {
  float fx = ffloor(x), fy = ffloor(y);
  i32 ix = (i32)fx, iy = (i32)fy;
  float tx = x - fx, ty = y - fy;
  tx = tx * tx * (3.f - 2.f * tx); ty = ty * ty * (3.f - 2.f * ty);
  float a = hash2(ix, iy), b = hash2(ix + 1, iy), c = hash2(ix, iy + 1), e = hash2(ix + 1, iy + 1);
  return mixf(mixf(a, b, tx), mixf(c, e, tx), ty);
}
static float fbm(float x, float y) {
  float s = 0.f, a = 0.5f;
  for (int i = 0; i < 4; ++i) { s += a * vnoise(x, y); x = x * 2.03f + 17.1f; y = y * 2.03f - 9.7f; a *= 0.5f; }
  return s;
}
static float gridLine(float u, float w) { float f = fract(u); return (f < w || f > 1.f - w) ? 1.f : 0.f; }

static V3 texAlbedo(const Material& m, V3 p, V3 n) {
  switch (m.tex) {
    case T_ISLAND: {
      if (fabsx(n.y) < 0.5f) {                                     // seawall
        V3 c = V3(0.42f, 0.41f, 0.39f) * (0.8f + 0.25f * vnoise(p.x * 0.6f + p.z * 0.6f, p.y * 3.f));
        if (fract(p.y * 1.25f) < 0.04f) c *= 0.6f;
        if (p.y < 0.12f) c *= 0.5f;                                 // wet line
        return c;
      }
      float r = fsqrt(p.x * p.x + p.z * p.z);
      if (r > 21.5f) {                                              // promenade pavers
        if (gridLine(p.x / 1.5f, 0.012f) + gridLine(p.z / 1.5f, 0.012f) > 0.f) return V3(0.3f, 0.29f, 0.27f);
        float h = hash2((i32)ffloor(p.x / 1.5f), (i32)ffloor(p.z / 1.5f));
        return V3(0.62f, 0.60f, 0.56f) * (0.88f + 0.12f * h);
      }
      bool poolPad = fabsx(p.x) < 9.6f && p.z > 5.f && p.z < 13.4f;  // terrace extends around the pool
      bool carPad  = p.x > 13.f && p.x < 16.8f && fabsx(p.z) < 3.2f;  // and under the parked car
      if (r > 13.6f && !poolPad && !carPad) {                       // lawn
        float g = fbm(p.x * 0.6f, p.z * 0.6f);
        V3 c = mix(V3(0.05f, 0.15f, 0.03f), V3(0.16f, 0.17f, 0.06f), g * g);
        return c * (0.7f + 0.6f * vnoise(p.x * 9.f, p.z * 9.f));
      }
      if (gridLine(p.x, 0.015f) + gridLine(p.z + 0.5f * fract(ffloor(p.x) * 0.5f) * 2.f, 0.015f) > 0.f)
        return V3(0.22f, 0.22f, 0.21f);                             // stone terrace
      return V3(0.56f, 0.55f, 0.52f) * (0.84f + 0.24f * hash2((i32)ffloor(p.x), (i32)ffloor(p.z)));
    }
    case T_WOOD: {
      float u = fabsx(n.y) > 0.5f ? p.z : (fabsx(n.x) > 0.5f ? p.z : p.x);
      float v = fabsx(n.y) > 0.5f ? p.x : p.y;
      float plank = ffloor(u * 5.f);
      float h = hash2((i32)plank, 7);
      float grain = 0.88f + 0.12f * fsin(v * 30.f + h * 40.f + vnoise(u * 3.f, v * 6.f) * 5.f);
      V3 c = m.albedo * (0.7f + 0.55f * h) * grain;
      if (fract(u * 5.f) < 0.05f) c *= 0.45f;
      return c;
    }
    case T_CONCRETE: {
      float u = fabsx(n.x) > 0.5f ? p.z : p.x;
      float v = fabsx(n.y) > 0.5f ? p.z : p.y;
      V3 c = m.albedo * (0.93f + 0.09f * vnoise(u * 2.f + v, v * 2.f - u));
      if (gridLine(u / 1.6f, 0.006f) + gridLine(v / 1.6f, 0.006f) > 0.f) c *= 0.82f;
      return c;
    }
    case T_POOL: {
      float u = fabsx(n.y) > 0.5f ? p.x : (fabsx(n.x) > 0.5f ? p.z : p.x);   // walls tile in (horizontal, y)
      float v = fabsx(n.y) > 0.5f ? p.z : p.y;
      if (gridLine(u * 5.f, 0.06f) + gridLine(v * 5.f, 0.06f) > 0.f) return V3(0.85f, 0.88f, 0.88f);
      return m.albedo * (0.88f + 0.14f * hash2((i32)ffloor(u * 5.f), (i32)ffloor(v * 5.f)));
    }
    case T_SAND: {
      float f = fbm(p.x * 0.35f, p.z * 0.35f);
      return m.albedo * (0.7f + 0.4f * f) * (0.92f + 0.08f * fsin(p.x * 2.6f + f * 6.f));
    }
    case T_FOLIAGE: {                                               // clumpy leaves, dark gaps, sun-bleached tips
      float big = vnoise(p.x * 2.f + p.y * 1.5f, p.z * 2.f - p.y * 1.3f);
      float leaf = vnoise(p.x * 11.f + p.y * 7.f, p.z * 11.f - p.y * 9.f);
      float fine = vnoise(p.x * 27.f - p.z * 5.f, p.y * 27.f + p.x * 5.f);
      float f = 0.5f * big + 0.35f * leaf + 0.15f * fine;
      V3 c = m.albedo * (0.35f + 1.15f * f * f * 1.6f);
      return mix(c, c * V3(1.5f, 1.35f, 0.8f), sat(n.y * 0.5f + 0.1f) * f);
    }
    case T_BARK: {                                                  // vertical furrows with irregular breaks
      float a = fatan2(p.z, p.x);
      float ridge = vnoise(a * 7.f + p.x * 0.5f, p.y * 1.4f + p.z * 0.5f);
      float crack = vnoise(a * 19.f, p.y * 3.f);
      return m.albedo * (0.5f + 0.9f * ridge) * (0.75f + 0.5f * crack);
    }
    default: return m.albedo;
  }
}
