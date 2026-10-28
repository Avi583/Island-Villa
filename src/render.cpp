// ─────────────────────────────────────────────────────────────────────────────
// render.cpp — the ray tracer: sky model, procedural textures, water optics,
// direct + indirect lighting, ACES tone mapping and the WebAssembly ABI.
//
// Each Web Worker instantiates this module and renders an interleaved set of
// rows (rowStart, rowStart + rowStep, …) so work is balanced between sky-heavy
// and geometry-heavy parts of the frame.
// ─────────────────────────────────────────────────────────────────────────────
#include "scene.h"
#include "runtime.h"
#include "frame.h"
#include "sky.h"
#include "texture.h"
#include "water.h"
#include "lighting.h"
#include "tonemap.h"

using namespace rt;

// ── per-frame state ──────────────────────────────────────────────────────────
static u32*   g_out;
static float* g_accum;
static float* g_depth;                 // per-pixel reprojection distance (see tracePath)
static u32    g_cap;

static void setupFrame(float time, float elev, float az, i32 shadows) {
  g_f.time = time;
  g_f.shadows = shadows;
  float se = fsin(elev), ce = fcos(elev);
  g_f.sunDir = normalize(V3(ce * fcos(az), se, ce * fsin(az)));
  float day = sat(se * 4.f + 0.3f);
  float golden = 1.f - sat(se * 2.5f);
  g_f.day = day;
  g_f.night = sat(-se * 5.f);                      // stars only once the sun has set
  float sunI = 3.2f * sat(se * 6.f + 0.08f);
  g_f.sunCol = mix(V3(1.f, 0.94f, 0.86f), V3(1.f, 0.58f, 0.28f), golden) * sunI;
  // Three-stop gradient: warm horizon → pale band → blue zenith (avoids the purple "mix" midtone)
  V3 zDay   = mix(V3(0.09f, 0.22f, 0.55f), V3(0.10f, 0.20f, 0.46f), golden);
  V3 mDay   = mix(V3(0.38f, 0.55f, 0.85f), V3(0.50f, 0.56f, 0.66f), golden);
  V3 hSun   = mix(V3(0.62f, 0.74f, 0.92f), V3(1.05f, 0.62f, 0.32f), golden);
  V3 hAway  = mix(V3(0.58f, 0.70f, 0.90f), V3(0.48f, 0.52f, 0.62f), golden);
  V3 zNight = V3(0.004f, 0.007f, 0.022f), hNight = V3(0.03f, 0.04f, 0.08f);
  g_f.zenith      = mix(zNight, zDay, day);
  g_f.midSky      = mix(mix(zNight, hNight, 0.5f), mDay, day);
  g_f.horizon     = mix(hNight * 1.3f, hSun, day);
  g_f.horizonAway = mix(hNight, hAway, day);
}

// Cheap closed-form shading for what is seen in a reflection.
static V3 shadeReflection(Ray r, Rng& g, bool shadows) {
  V3 T(1.f);
  for (int i = 0; i < 4; ++i) {
    Hit h;
    if (!intersectScene(r, INF, h)) return T * skyColor(r.d, true);
    const Material& m = g_mats[h.mat];
    V3 p = r.o + r.d * h.t;
    V3 n = dot(h.n, r.d) < 0.f ? h.n : -h.n;
    float fog = 1.f - fexp(-fmaxx(h.t - 40.f, 0.f) * 0.002f);
    switch (m.kind) {
      case K_EMIT: return T * m.emit;
      case K_GLASS: T *= m.albedo * 0.92f; r = makeRay(p + r.d * EPS, r.d); continue;
      case K_METAL: T *= m.albedo; r = makeRay(p + n * EPS, reflect(r.d, n)); continue;
      case K_WATER: {
        V3 wn = waterNormal(p, h.mat, n, h.t);
        float F = schlick(0.02f, fabsx(dot(r.d, wn)));
        V3 deep = (h.mat == M_POOL ? V3(0.05f, 0.30f, 0.34f) : V3(0.012f, 0.05f, 0.055f))
                * (luminance(g_f.sunCol) * 0.35f + 0.15f);
        V3 wc = skyColor(reflect(r.d, wn), true) * F + deep * (1.f - F);
        return T * (fog > 0.f ? mix(wc, hazeColor(r.d), fog) : wc);
      }
      default: {
        V3 alb = texAlbedo(m, p, n);
        V3 c = alb * (directLight(p, n, g, shadows) + ambientCheap(n)) + emission(m, h.mat, p);
        if (m.kind == K_COAT) {
          float F = schlick(m.f0, fabsx(dot(r.d, n)));
          c = c * (1.f - F) + skyColor(reflect(r.d, n), false) * F;
        }
        return T * (fog > 0.f ? mix(c, hazeColor(r.d), fog) : c);
      }
    }
  }
  return T * skyColor(r.d, false);
}

// ── the main path ────────────────────────────────────────────────────────────
// reproT receives the distance along the camera ray to the first non-glass surface (1e5 for
// sky). Thin glass doesn't bend rays, so the interior seen through a pane reprojects with its
// own depth instead of sticking to the pane when the GPU temporal pass reuses old frames.
static V3 tracePath(Ray r, Rng& g, float gi, float& reproT) {
  const bool shadows = g_f.shadows != 0;
  V3 L(0.f), T(1.f), sigma(0.f);
  float firstT = -1.f;
  V3 firstDir = r.d;
  bool straight = true;                                         // still on the camera ray (only glass so far)
  reproT = 0.f;

  for (int depth = 0; depth < 8; ++depth) {
    Hit h;
    bool hit = intersectScene(r, INF, h);
    float t = hit ? h.t : 1e5f;
    if (sigma.x > 0.f) T *= exp3(sigma * (-fminx(t, 60.f)));   // Beer–Lambert inside water
    if (depth == 0) firstT = t;
    if (!hit) {
      if (straight) reproT = 1e5f;
      L += T * skyColor(r.d, true);
      break;
    }

    const Material& m = g_mats[h.mat];
    if (straight) {
      reproT += h.t;
      if (m.kind == K_GLASS) reproT += 2.f * EPS; else straight = false;
    }
    V3 p = r.o + r.d * h.t;
    bool front = dot(h.n, r.d) < 0.f;
    V3 nf = front ? h.n : -h.n;

    if (m.kind == K_EMIT) { L += T * m.emit; break; }

    if (m.kind == K_GLASS) {                                    // thin glass
      if (front) {
        float F = schlick(m.f0, -dot(r.d, nf));
        if (depth < 4) L += T * F * shadeReflection(makeRay(p + nf * EPS, reflect(r.d, nf)), g, shadows && depth < 2);
        T *= m.albedo * (1.f - F);
      }
      r = makeRay(p + r.d * (2.f * EPS), r.d);
      continue;
    }

    if (m.kind == K_WATER) {                                    // dielectric interface
      V3 wn = waterNormal(p, h.mat, nf, h.t);
      if (dot(wn, r.d) > -0.02f) wn = nf;
      float eta = front ? 1.f / 1.333f : 1.333f;
      V3 rd = reflect(r.d, wn), td;
      if (!refract(r.d, wn, eta, td)) { r = makeRay(p + wn * EPS, rd); continue; }   // total internal reflection
      // Schlick uses the angle on the optically thinner (air) side.
      float F = schlick(0.02f, front ? -dot(r.d, wn) : -dot(td, wn));
      if (front && depth < 4) L += T * F * shadeReflection(makeRay(p + wn * EPS, rd), g, shadows && depth < 2);
      T *= (1.f - F);
      sigma = front ? m.sigma : V3(0.f);
      r = makeRay(p - wn * (2.f * EPS), td);
      continue;
    }

    if (m.kind == K_METAL) {
      V3 rd = reflect(r.d, nf);
      if (m.rough > 0.f) {
        rd = normalize(rd + V3(g.next() - 0.5f, g.next() - 0.5f, g.next() - 0.5f) * (2.f * m.rough));
        if (dot(rd, nf) < 0.f) rd = reflect(rd, nf);
      }
      T *= m.albedo;
      r = makeRay(p + nf * EPS, rd);
      continue;
    }

    // Diffuse / clear-coat: terminate here with full lighting.
    V3 alb = texAlbedo(m, p, nf);
    V3 E = directLight(p, nf, g, shadows);
    V3 amb = ambientCheap(nf);
    if (gi > 0.f) amb = amb * (1.f - gi) + indirectGI(p, nf, g, shadows) * gi;
    V3 c = alb * (E + amb) + emission(m, h.mat, p);
    if (m.kind == K_COAT) {
      float F = schlick(m.f0, -dot(r.d, nf));
      V3 rd = reflect(r.d, nf);
      V3 refl = depth < 4 ? shadeReflection(makeRay(p + nf * EPS, rd), g, shadows && depth < 2) : skyColor(rd, false);
      c = c * (1.f - F) + refl * F;
    }
    L += T * c;
    break;
  }

  if (firstT < 9e4f) {                                          // aerial perspective
    float fog = 1.f - fexp(-fmaxx(firstT - 30.f, 0.f) * 0.0022f);
    if (fog > 0.f) L = mix(L, hazeColor(firstDir), fog);
  }
  return L;
}

// ── exported API ─────────────────────────────────────────────────────────────
RT_EXPORT(rt_init) void rt_init() { buildGammaLut(); buildScene(); }

// Reserve storage for `pixels` output pixels (RGBA8 + float RGB accumulator + float depth).
RT_EXPORT(rt_alloc) u32* rt_alloc(u32 pixels) {
  if (pixels > g_cap) {
    unsigned char* mem = heapAlloc(pixels * 20u);
    if (!mem) return nullptr;
    g_out = (u32*)mem;
    g_accum = (float*)(mem + pixels * 4u);
    g_depth = (float*)(mem + pixels * 16u);
    g_cap = pixels;
  }
  for (u32 i = 0; i < pixels * 3u; ++i) g_accum[i] = 0.f;
  return g_out;
}
RT_EXPORT(rt_out) u32* rt_out() { return g_out; }
RT_EXPORT(rt_depth) float* rt_depth() { return g_depth; }

// Renders rows y = rowStart, rowStart+rowStep, … into a compact buffer (local row i at i*w).
// blend: 1 = replace, <1 = exponential temporal accumulation (progressive refinement).
RT_EXPORT(rt_render) void rt_render(i32 w, i32 h, i32 rowStart, i32 rowStep, float time,
                                    float cx, float cy, float cz, float yaw, float pitch, float fov,
                                    i32 frame, float blend, float sunElev, float sunAz,
                                    float exposure, float gi, i32 shadows) {
  if (!g_out) return;
  setupFrame(time, sunElev, sunAz, shadows);

  V3 cam(cx, cy, cz);
  V3 fwd = normalize(V3(fcos(pitch) * fcos(yaw), fsin(pitch), fcos(pitch) * fsin(yaw)));
  V3 right = normalize(cross(fwd, V3(0.f, 1.f, 0.f)));
  V3 up = cross(right, fwd);
  float th = fsin(fov * 0.5f) / fcos(fov * 0.5f);
  float aspect = (float)w / (float)h;

  i32 local = 0;
  for (i32 y = rowStart; y < h; y += rowStep, ++local) {
    if ((u32)((local + 1) * w) > g_cap) break;
    for (i32 x = 0; x < w; ++x) {
      u32 idx = (u32)(local * w + x);
      Rng g; g.s = hash32((u32)x * 1973u + (u32)y * 9277u + (u32)frame * 26699u) | 1u;
      float u = ((x + g.next()) / w) * 2.f - 1.f;
      float v = 1.f - ((y + g.next()) / h) * 2.f;
      V3 dir = normalize(fwd + right * (u * th * aspect) + up * (v * th));
      V3 c = tracePath(makeRay(cam, dir), g, gi, g_depth[idx]);
      if (!(c.x == c.x) || !(c.y == c.y) || !(c.z == c.z)) c = V3(0.f);   // NaN guard
      float lc = luminance(c);
      if (lc > 60.f) c *= 60.f / lc;

      float* a = g_accum + idx * 3u;
      V3 acc(a[0], a[1], a[2]);
      acc = blend >= 1.f ? c : acc + (c - acc) * blend;
      a[0] = acc.x; a[1] = acc.y; a[2] = acc.z;

      float vig = 1.f - 0.22f * (u * u * 0.6f + v * v);
      V3 o = acc * (exposure * vig);
      float dz = g.next() - 0.5f;
      g_out[idx] = toSRGB8(o.x, dz) | (toSRGB8(o.y, dz) << 8) | (toSRGB8(o.z, dz) << 16) | 0xFF000000u;
    }
  }
}
