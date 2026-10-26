// ─────────────────────────────────────────────────────────────────────────────
// render.cpp — the ray tracer: sky model, procedural textures, water optics,
// direct + indirect lighting, ACES tone mapping and the WebAssembly ABI.
//
// Each Web Worker instantiates this module and renders an interleaved set of
// rows (rowStart, rowStart + rowStep, …) so work is balanced between sky-heavy
// and geometry-heavy parts of the frame.
// ─────────────────────────────────────────────────────────────────────────────
#include "scene.h"

using namespace rt;

#ifdef __wasm__
#define RT_EXPORT(name) extern "C" __attribute__((export_name(#name)))
extern "C" {
// The compiler may emit calls to these for struct copies; we have no libc.
void* memset(void* d, int c, unsigned long n) {
  volatile unsigned char* p = (volatile unsigned char*)d;
  while (n--) *p++ = (unsigned char)c;
  return d;
}
void* memcpy(void* d, const void* s, unsigned long n) {
  volatile unsigned char* p = (volatile unsigned char*)d;
  const unsigned char* q = (const unsigned char*)s;
  while (n--) *p++ = *q++;
  return d;
}
void* memmove(void* d, const void* s, unsigned long n) {
  volatile unsigned char* p = (volatile unsigned char*)d;
  const unsigned char* q = (const unsigned char*)s;
  if (p < q) { while (n--) *p++ = *q++; }
  else { p += n; q += n; while (n--) *--p = *--q; }
  return d;
}
extern unsigned char __heap_base;
}
// Bump allocator straight on top of linear memory: grow pages as needed.
static unsigned char* heapAlloc(u32 bytes) {
  u32 base = ((u32)(unsigned long)&__heap_base + 15u) & ~15u;
  u32 need = base + bytes;
  u32 have = (u32)__builtin_wasm_memory_size(0) * 65536u;
  if (need > have) {
    u32 pages = (need - have + 65535u) / 65536u;
    if (__builtin_wasm_memory_grow(0, pages) == (unsigned long)-1) return nullptr;
  }
  return (unsigned char*)(unsigned long)base;
}
#else
#define RT_EXPORT(name) extern "C"
extern "C" void* malloc(unsigned long);
static unsigned char* heapAlloc(u32 bytes) { return (unsigned char*)malloc(bytes); }
#endif

// ── per-frame state ──────────────────────────────────────────────────────────
struct Frame {
  V3 sunDir, sunCol, zenith, horizon, horizonAway, midSky;
  float time, night, day;
  i32 shadows;
};
static Frame g_f;

static u32*   g_out;
static float* g_accum;
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
      if (r > ROAD_OUT) {                                           // promenade pavers
        if (gridLine(p.x / 1.5f, 0.012f) + gridLine(p.z / 1.5f, 0.012f) > 0.f) return V3(0.3f, 0.29f, 0.27f);
        float h = hash2((i32)ffloor(p.x / 1.5f), (i32)ffloor(p.z / 1.5f));
        return V3(0.62f, 0.60f, 0.56f) * (0.88f + 0.12f * h);
      }
      if (r > ROAD_IN) {                                            // asphalt ring road
        float ang = fatan2(p.z, p.x);
        if (fabsx(r - 19.5f) < 0.07f && fract(ang * 19.5f / 3.f) < 0.5f) return V3(0.78f, 0.66f, 0.28f);
        if (fabsx(r - 17.75f) < 0.06f || fabsx(r - 21.25f) < 0.06f) return V3(0.72f);
        return V3(0.055f, 0.055f, 0.06f) * (0.75f + 0.5f * vnoise(p.x * 4.f, p.z * 4.f));
      }
      if (r > 13.6f) {                                              // lawn
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
static V3 tracePath(Ray r, Rng& g, float gi) {
  const bool shadows = g_f.shadows != 0;
  V3 L(0.f), T(1.f), sigma(0.f);
  float firstT = -1.f;
  V3 firstDir = r.d;

  for (int depth = 0; depth < 8; ++depth) {
    Hit h;
    bool hit = intersectScene(r, INF, h);
    float t = hit ? h.t : 1e5f;
    if (sigma.x > 0.f) T *= exp3(sigma * (-fminx(t, 60.f)));   // Beer–Lambert inside water
    if (depth == 0) firstT = t;
    if (!hit) { L += T * skyColor(r.d, true); break; }

    const Material& m = g_mats[h.mat];
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

// ── tone mapping ─────────────────────────────────────────────────────────────
static float aces(float x) {           // Narkowicz ACES fit
  return sat((x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f));
}
static u32 toSRGB8(float x, float dither) {
  float v = fpow(aces(x), 1.f / 2.2f) * 255.f + dither;
  return (u32)clampf(v, 0.f, 255.f);
}

// ── exported API ─────────────────────────────────────────────────────────────
RT_EXPORT(rt_init) void rt_init() { buildScene(); }

// Reserve storage for `pixels` output pixels (RGBA8 + float RGB accumulator).
RT_EXPORT(rt_alloc) u32* rt_alloc(u32 pixels) {
  if (pixels > g_cap) {
    unsigned char* mem = heapAlloc(pixels * 16u);
    if (!mem) return nullptr;
    g_out = (u32*)mem;
    g_accum = (float*)(mem + pixels * 4u);
    g_cap = pixels;
  }
  for (u32 i = 0; i < pixels * 3u; ++i) g_accum[i] = 0.f;
  return g_out;
}
RT_EXPORT(rt_out) u32* rt_out() { return g_out; }

// Renders rows y = rowStart, rowStart+rowStep, … into a compact buffer (local row i at i*w).
// blend: 1 = replace, <1 = exponential temporal accumulation (progressive refinement).
RT_EXPORT(rt_render) void rt_render(i32 w, i32 h, i32 rowStart, i32 rowStep, float time,
                                    float cx, float cy, float cz, float yaw, float pitch, float fov,
                                    i32 frame, float blend, float sunElev, float sunAz,
                                    float exposure, float gi, i32 shadows) {
  if (!g_out) return;
  setupFrame(time, sunElev, sunAz, shadows);
  updateDynamic(time);

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
      V3 c = tracePath(makeRay(cam, dir), g, gi);
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
