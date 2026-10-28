// lighting.h — direct light, procedural emission and one-bounce indirect GI.
#pragma once
#include "scene.h"

using namespace rt;

#include "frame.h"
#include "sky.h"
#include "texture.h"

// ── lighting ─────────────────────────────────────────────────────────────────
static V3 directLight(V3 p, V3 n, Rng& g, bool shadows) {
  V3 E(0.f);
  if (g_f.sunCol.x > 1e-4f) {
    V3 L = coneJitter(g_f.sunDir, 0.016f, g);        // area sun → soft penumbrae
    float ndl = dot(n, L);
    if (ndl > 0.f && !(shadows && occluded(makeRay(p + n * (2.f * EPS), L), INF))) {
      V3 c = g_f.sunCol * ndl;
      if (p.y < LAKE_Y - 0.01f && p.x * p.x + p.z * p.z > ISLAND_R * ISLAND_R)
        c *= exp3(g_mats[M_LAKE].sigma * (p.y / fmaxx(L.y, 0.08f)));   // light lost travelling down through the lake
      E += c;
    }
  }
  for (i32 i = 0; i < g_lightCount; ++i) {
    const Light& l = g_lights[i];
    V3 v = l.p - p;
    float d2 = dot(v, v), r2 = l.range * l.range;
    if (d2 >= r2) continue;
    float d = fsqrt(d2);
    float ndl = dot(n, v) / d;
    if (ndl <= 0.f) continue;
    float w = 1.f - d2 / r2; w *= w;
    E += l.col * (ndl * w / (1.f + d2));
  }
  return E;
}

// Emission with procedural detail: the upper floor's tinted ribbon window shows
// mullions every 1.4 m and individually lit rooms of varying warmth behind the glass.
static V3 emission(const Material& m, i32 mat, V3 p) {
  if (mat != M_TINT) return m.emit;
  bool longSide = fabsx(fabsx(p.z) - 6.64f) < 0.1f;
  float u = longSide ? p.x : p.z;
  float f = fract(u / 1.4f);
  if (f < 0.035f || f > 0.965f) return V3(0.f);
  float h = hash2((i32)ffloor(u / 1.4f), longSide ? (p.z > 0.f ? 3 : 5) : (p.x > -5.f ? 7 : 9));
  float lit = h > 0.3f ? (0.45f + 0.9f * h) : 0.08f;
  float grad = 0.45f + 0.55f * smoothstep(5.0f, 6.9f, p.y);          // light falls from the ceiling
  return m.emit * (0.75f * lit * grad);
}

// One-bounce diffuse global illumination along a cosine-weighted direction.
static V3 indirectGI(V3 p, V3 n, Rng& g, bool shadows) {
  V3 dir = cosineSample(n, g);
  Ray r = makeRay(p + n * (2.f * EPS), dir);
  Hit h;
  if (!intersectScene(r, 300.f, h)) return skyColor(dir, false);
  const Material& m = g_mats[h.mat];
  if (m.kind == K_EMIT) {
    // Small, very bright emitters are already represented by point lights; clamping
    // their bounce contribution removes fireflies without visibly changing the image.
    float le = luminance(m.emit);
    return le > 0.08f ? m.emit * (0.08f / le) : m.emit;
  }
  if (m.kind == K_GLASS || m.kind == K_WATER) return skyColor(dir, false) * 0.5f;
  V3 hp = r.o + r.d * h.t;
  V3 hn = dot(h.n, dir) > 0.f ? -h.n : h.n;
  V3 alb = texAlbedo(m, hp, hn);
  float ndl = dot(hn, g_f.sunDir);
  V3 E = ambientCheap(hn) * 0.6f;
  if (ndl > 0.f && !(shadows && occluded(makeRay(hp + hn * (2.f * EPS), g_f.sunDir), INF))) E += g_f.sunCol * ndl;
  V3 c = alb * E;
  float lc = luminance(c);
  return lc > 4.f ? c * (4.f / lc) : c;               // firefly clamp
}
