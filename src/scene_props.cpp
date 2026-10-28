// scene_props.cpp — procedural palm trees.
#include "scene_builder.h"

namespace rt {

// ── palm trees: tall curved tapered trunk, ringed bulges, drooping fronds, coconut cluster ──
static float treeRand(u32& s) {                                 // xorshift → [0,1)
  s ^= s << 13; s ^= s >> 17; s ^= s << 5;
  return (float)(s & 0xFFFFFFu) * (1.f / 16777216.f);
}
void buildTree(float x, float z, u32 seed) {
  u32 s = seed * 2654435761u + 12345u;
  for (int i = 0; i < 4; ++i) treeRand(s);
  const float base = 0.5f;
  float H    = 5.0f + 1.6f * treeRand(s);                       // trunk height
  float r0   = 0.20f + 0.04f * treeRand(s);                     // base radius
  float lean = 0.7f + 0.8f * treeRand(s), la = TAU * treeRand(s);
  float lx = lean * fcos(la), lz = lean * fsin(la);             // total horizontal drift at the top

  S(x, base + 0.05f, z, r0 * 1.6f, M_TRUNK);                    // swollen base
  const int N = 16;                                             // trunk: overlapping spheres on a curve
  float tx = x, ty = base, tz = z, tr = r0;
  for (int i = 0; i <= N; ++i) {
    float t = (float)i / N, bend = t * t;
    tx = x + lx * bend; tz = z + lz * bend; ty = base + H * t;
    tr = r0 * (1.f - 0.4f * t) * (i & 1 ? 1.12f : 1.f);         // alternating bulges read as trunk rings
    S(tx, ty, tz, tr, M_TRUNK);
  }

  // crown: fronds radiate from the top, rising then drooping under their own weight
  const int FRONDS = 9, SEG = 7;
  for (int i = 0; i < FRONDS; ++i) {
    float ang = TAU * (i + 0.4f * treeRand(s)) / FRONDS;
    float len = 2.0f + 0.7f * treeRand(s), rise = 0.9f + 0.5f * treeRand(s), droop = 1.8f + 0.8f * treeRand(s);
    float dx = fcos(ang), dz = fsin(ang);
    for (int j = 1; j <= SEG; ++j) {
      float u = (float)j / SEG;
      S(tx + dx * len * u, ty + 0.15f + rise * u - droop * u * u, tz + dz * len * u,
        0.30f * (1.f - 0.65f * u) + 0.05f, (i & 1) ? M_FOLIAGE2 : M_FOLIAGE);
    }
  }
  S(tx, ty + 0.2f, tz, 0.35f, M_FOLIAGE);                       // crown heart
  for (int i = 0; i < 4; ++i) {                                 // coconuts under the crown
    float ang = TAU * (i + treeRand(s)) / 4.f;
    S(tx + 0.28f * fcos(ang), ty - 0.2f, tz + 0.28f * fsin(ang), 0.17f, M_TRUNK);
  }
}

}  // namespace rt
