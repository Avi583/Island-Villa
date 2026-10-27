#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// mathx.h — freestanding math for a libc-free WebAssembly build.
//
// No <cmath>, no runtime, no allocations. Every function lowers either to a
// native WASM instruction (f32.sqrt, f32.floor, f32.abs …) or to a short
// polynomial, so the whole renderer ships as one small, dependency-free .wasm.
// ─────────────────────────────────────────────────────────────────────────────
typedef unsigned int u32;
typedef int i32;

#define RT_INLINE static inline __attribute__((always_inline))

namespace rt {

constexpr float PI     = 3.14159265358979f;
constexpr float TAU    = 6.28318530717959f;
constexpr float INF    = 1e30f;
constexpr float EPS    = 1e-3f;

// ── scalar ───────────────────────────────────────────────────────────────────
RT_INLINE float fsqrt(float x)  { return __builtin_sqrtf(x); }
RT_INLINE float fabsx(float x)  { return __builtin_fabsf(x); }
RT_INLINE float ffloor(float x) { return __builtin_floorf(x); }
RT_INLINE float fminx(float a, float b) { return a < b ? a : b; }
RT_INLINE float fmaxx(float a, float b) { return a > b ? a : b; }
RT_INLINE float clampf(float x, float a, float b) { return x < a ? a : (x > b ? b : x); }
RT_INLINE float sat(float x)    { return clampf(x, 0.f, 1.f); }
RT_INLINE float mixf(float a, float b, float t) { return a + (b - a) * t; }
RT_INLINE float fract(float x)  { return x - ffloor(x); }
RT_INLINE float smoothstep(float a, float b, float x) {
  float t = sat((x - a) / (b - a));
  return t * t * (3.f - 2.f * t);
}

// sin: reduce to [-pi,pi], fold to [-pi/2,pi/2], 9th-order odd polynomial (|err| < 4e-6)
RT_INLINE float fsin(float x) {
  x -= TAU * ffloor(x * (1.f / TAU) + 0.5f);
  if (x > 0.5f * PI)       x = PI - x;
  else if (x < -0.5f * PI) x = -PI - x;
  float x2 = x * x;
  return x * (1.f + x2 * (-1.f / 6.f + x2 * (1.f / 120.f + x2 * (-1.f / 5040.f + x2 * (1.f / 362880.f)))));
}
RT_INLINE float fcos(float x) { return fsin(x + 0.5f * PI); }

// 2^x: build the exponent directly in the IEEE-754 bits, polynomial for the fraction
RT_INLINE float fexp2(float x) {
  x = clampf(x, -126.f, 126.f);
  float fi = ffloor(x), f = x - fi;
  float p = 1.f + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f + f * (0.0096181f + f * 0.0013334f))));
  u32 bits = (u32)((i32)fi + 127) << 23;
  return p * __builtin_bit_cast(float, bits);
}
RT_INLINE float fexp(float x) { return fexp2(x * 1.44269504f); }

// log2: exponent from the bits, atanh series on the mantissa in [1,2)
RT_INLINE float flog2(float x) {
  u32 bits = __builtin_bit_cast(u32, x);
  float e  = (float)((i32)((bits >> 23) & 255u) - 127);
  float m  = __builtin_bit_cast(float, (bits & 0x007FFFFFu) | 0x3F800000u);
  float t  = (m - 1.f) / (m + 1.f), t2 = t * t;
  float ln = 2.f * t * (1.f + t2 * (1.f / 3.f + t2 * (1.f / 5.f + t2 * (1.f / 7.f + t2 * (1.f / 9.f)))));
  return e + ln * 1.44269504f;
}
RT_INLINE float fpow(float a, float b) { return a <= 0.f ? 0.f : fexp2(b * flog2(a)); }

RT_INLINE float fatanUnit(float z) {  // |z| <= 1, max err ~1e-5
  float z2 = z * z;
  return z * (0.99997726f + z2 * (-0.33262347f + z2 * (0.19354346f + z2 * (-0.11643287f + z2 * (0.05265332f + z2 * -0.01172120f)))));
}
RT_INLINE float fatan2(float y, float x) {
  float ax = fabsx(x), ay = fabsx(y);
  if (ax < 1e-20f && ay < 1e-20f) return 0.f;
  float a = ax > ay ? fatanUnit(ay / ax) : 0.5f * PI - fatanUnit(ax / ay);
  if (x < 0.f) a = PI - a;
  return y < 0.f ? -a : a;
}

// ── vector ───────────────────────────────────────────────────────────────────
struct V3 {
  float x, y, z;
  V3() = default;                                   // trivial → zero-cost static arrays
  constexpr V3(float a, float b, float c) : x(a), y(b), z(c) {}
  constexpr explicit V3(float s) : x(s), y(s), z(s) {}
  float operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
};

RT_INLINE V3 operator+(V3 a, V3 b)   { return V3(a.x + b.x, a.y + b.y, a.z + b.z); }
RT_INLINE V3 operator-(V3 a, V3 b)   { return V3(a.x - b.x, a.y - b.y, a.z - b.z); }
RT_INLINE V3 operator-(V3 a)         { return V3(-a.x, -a.y, -a.z); }
RT_INLINE V3 operator*(V3 a, V3 b)   { return V3(a.x * b.x, a.y * b.y, a.z * b.z); }
RT_INLINE V3 operator*(V3 a, float s){ return V3(a.x * s, a.y * s, a.z * s); }
RT_INLINE V3 operator*(float s, V3 a){ return V3(a.x * s, a.y * s, a.z * s); }
RT_INLINE V3 operator/(V3 a, float s){ float i = 1.f / s; return a * i; }
RT_INLINE V3& operator+=(V3& a, V3 b)   { a = a + b; return a; }
RT_INLINE V3& operator*=(V3& a, V3 b)   { a = a * b; return a; }
RT_INLINE V3& operator*=(V3& a, float s){ a = a * s; return a; }

RT_INLINE float dot(V3 a, V3 b)  { return a.x * b.x + a.y * b.y + a.z * b.z; }
RT_INLINE V3 cross(V3 a, V3 b)   { return V3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
RT_INLINE float length(V3 a)     { return fsqrt(dot(a, a)); }
RT_INLINE V3 normalize(V3 a)     { float l2 = dot(a, a); return l2 > 1e-20f ? a * (1.f / fsqrt(l2)) : V3(0.f, 1.f, 0.f); }
RT_INLINE V3 vmin(V3 a, V3 b)    { return V3(fminx(a.x, b.x), fminx(a.y, b.y), fminx(a.z, b.z)); }
RT_INLINE V3 vmax(V3 a, V3 b)    { return V3(fmaxx(a.x, b.x), fmaxx(a.y, b.y), fmaxx(a.z, b.z)); }
RT_INLINE V3 mix(V3 a, V3 b, float t) { return a + (b - a) * t; }
RT_INLINE V3 exp3(V3 a)          { return V3(fexp(a.x), fexp(a.y), fexp(a.z)); }
RT_INLINE V3 reflect(V3 d, V3 n) { return d - n * (2.f * dot(d, n)); }
RT_INLINE float luminance(V3 c)  { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

// ── 4-wide SIMD (clang vector extensions → WASM simd128, no headers) ─────────
typedef float F4 __attribute__((vector_size(16)));
typedef i32   I4 __attribute__((vector_size(16)));   // comparisons yield lanes of 0 / -1

RT_INLINE F4 splat4(float s) { return F4{s, s, s, s}; }
RT_INLINE F4 sel4(I4 m, F4 a, F4 b) { return (F4)((m & (I4)a) | (~m & (I4)b)); }
// Same operand order and NaN behaviour as fminx / fmaxx, lane by lane.
RT_INLINE F4 vmin4(F4 a, F4 b) { return sel4(a < b, a, b); }
RT_INLINE F4 vmax4(F4 a, F4 b) { return sel4(a > b, a, b); }

// Snell refraction. n faces the incoming ray; eta = n1/n2. false on total internal reflection.
RT_INLINE bool refract(V3 d, V3 n, float eta, V3& out) {
  float cosi = -dot(n, d);
  float k = 1.f - eta * eta * (1.f - cosi * cosi);
  if (k < 0.f) return false;
  out = normalize(d * eta + n * (eta * cosi - fsqrt(k)));
  return true;
}
RT_INLINE float schlick(float f0, float cosT) {
  float m = 1.f - sat(cosT), m2 = m * m;
  return f0 + (1.f - f0) * m2 * m2 * m;
}

// Orthonormal basis from a unit normal (Duff et al. 2017, branchless)
RT_INLINE void onb(V3 n, V3& t, V3& b) {
  float s = n.z >= 0.f ? 1.f : -1.f;
  float a = -1.f / (s + n.z);
  float c = n.x * n.y * a;
  t = V3(1.f + s * n.x * n.x * a, s * c, -s * n.x);
  b = V3(c, s + n.y * n.y * a, -n.y);
}

// ── random ───────────────────────────────────────────────────────────────────
RT_INLINE u32 hash32(u32 x) {
  x ^= x >> 16; x *= 0x7feb352du;
  x ^= x >> 15; x *= 0x846ca68bu;
  x ^= x >> 16; return x;
}
struct Rng {                      // PCG-RXS-M-XS 32
  u32 s;
  inline float next() {
    s = s * 747796405u + 2891336453u;
    u32 w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    w = (w >> 22u) ^ w;
    return (float)(w >> 8) * (1.f / 16777216.f);
  }
};
RT_INLINE V3 cosineSample(V3 n, Rng& g) {
  V3 t, b; onb(n, t, b);
  float u1 = g.next(), u2 = g.next();
  float r = fsqrt(u1), ph = TAU * u2;
  return normalize(t * (r * fcos(ph)) + b * (r * fsin(ph)) + n * fsqrt(fmaxx(0.f, 1.f - u1)));
}
RT_INLINE V3 coneJitter(V3 d, float radius, Rng& g) {
  V3 t, b; onb(d, t, b);
  float r = radius * fsqrt(g.next()), ph = TAU * g.next();
  return normalize(d + t * (r * fcos(ph)) + b * (r * fsin(ph)));
}

}  // namespace rt
