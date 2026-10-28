#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// primitives.h — rays, hit records and analytic primitives (box, sphere, cylinder) with their
// intersection routines. All storage is fixed-size and static: no heap, no STL.
// ─────────────────────────────────────────────────────────────────────────────
#include "mathx.h"

namespace rt {

struct Ray { V3 o, d, inv; };
RT_INLINE Ray makeRay(V3 o, V3 d) {
  Ray r; r.o = o; r.d = d; r.inv = V3(1.f / d.x, 1.f / d.y, 1.f / d.z); return r;
}

struct Hit { float t; V3 n; i32 mat; i32 obj; };   // n is the OUTWARD surface normal

enum PrimType : i32 { P_BOX = 0, P_SPHERE = 1, P_CYL = 2 };
struct Prim { V3 a, b; i32 type, axis, mat, pad; };
//  box:      a = min corner, b = max corner
//  sphere:   a = centre, b.x = radius
//  cylinder: a = centre, b.x = radius, b.y = half height, axis = 0|1|2

// Rectangular opening cut through the island's top cap (the sunken pool). Empty by default.
struct HoleRect { float x0, z0, x1, z1; };
inline HoleRect g_hole = {0.f, 0.f, 0.f, 0.f};
RT_INLINE bool inHole(float x, float z) { return x > g_hole.x0 && x < g_hole.x1 && z > g_hole.z0 && z < g_hole.z1; }

RT_INLINE Prim mkBox(V3 mn, V3 mx, i32 mat) { Prim p; p.a = mn; p.b = mx; p.type = P_BOX; p.axis = 0; p.mat = mat; p.pad = 0; return p; }
RT_INLINE Prim mkSphere(V3 c, float r, i32 mat) { Prim p; p.a = c; p.b = V3(r, 0.f, 0.f); p.type = P_SPHERE; p.axis = 0; p.mat = mat; p.pad = 0; return p; }
RT_INLINE Prim mkCyl(V3 c, float r, float halfH, i32 axis, i32 mat) { Prim p; p.a = c; p.b = V3(r, halfH, 0.f); p.type = P_CYL; p.axis = axis; p.mat = mat; p.pad = 0; return p; }
//  a cylinder with pad = 1 (axis 1) has its top cap opened wherever g_hole covers it

RT_INLINE V3 axisVec(i32 axis, float s) { return axis == 0 ? V3(s, 0.f, 0.f) : (axis == 1 ? V3(0.f, s, 0.f) : V3(0.f, 0.f, s)); }

RT_INLINE void primBounds(const Prim& p, V3& mn, V3& mx) {
  if (p.type == P_BOX) { mn = p.a; mx = p.b; }
  else if (p.type == P_SPHERE) { V3 r(p.b.x); mn = p.a - r; mx = p.a + r; }
  else { V3 e = V3(p.b.x) - axisVec(p.axis, p.b.x) + axisVec(p.axis, p.b.y); mn = p.a - e; mx = p.a + e; }
}

// ── primitive intersection ───────────────────────────────────────────────────
RT_INLINE bool hitBox(const Prim& p, const Ray& r, float tmax, float& tOut, V3& nOut) {
  float tx1 = (p.a.x - r.o.x) * r.inv.x, tx2 = (p.b.x - r.o.x) * r.inv.x;
  float ty1 = (p.a.y - r.o.y) * r.inv.y, ty2 = (p.b.y - r.o.y) * r.inv.y;
  float tz1 = (p.a.z - r.o.z) * r.inv.z, tz2 = (p.b.z - r.o.z) * r.inv.z;
  float txn = fminx(tx1, tx2), txf = fmaxx(tx1, tx2);
  float tyn = fminx(ty1, ty2), tyf = fmaxx(ty1, ty2);
  float tzn = fminx(tz1, tz2), tzf = fmaxx(tz1, tz2);
  float tn = fmaxx(txn, fmaxx(tyn, tzn));
  float tf = fminx(txf, fminx(tyf, tzf));
  if (tf < tn || tf <= EPS) return false;
  float th; i32 ax; float sg;
  if (tn > EPS) { th = tn; ax = (tn == txn) ? 0 : ((tn == tyn) ? 1 : 2); sg = -1.f; }   // entering
  else          { th = tf; ax = (tf == txf) ? 0 : ((tf == tyf) ? 1 : 2); sg =  1.f; }   // leaving (ray starts inside)
  if (th >= tmax) return false;
  float dc = r.d[ax];
  tOut = th;
  nOut = axisVec(ax, sg * (dc < 0.f ? -1.f : 1.f));
  return true;
}

RT_INLINE bool hitSphere(const Prim& p, const Ray& r, float tmax, float& tOut, V3& nOut) {
  V3 oc = r.o - p.a;
  float R = p.b.x;
  float b = dot(oc, r.d), c = dot(oc, oc) - R * R;
  float disc = b * b - c;
  if (disc < 0.f) return false;
  float s = fsqrt(disc);
  float t = -b - s;
  if (t <= EPS) t = -b + s;
  if (t <= EPS || t >= tmax) return false;
  tOut = t;
  nOut = (r.o + r.d * t - p.a) * (1.f / R);
  return true;
}

RT_INLINE bool hitCyl(const Prim& p, const Ray& r, float tmax, float& tOut, V3& nOut) {
  i32 w = p.axis, u = (w + 1) % 3, v = (w + 2) % 3;
  float R = p.b.x, H = p.b.y;
  float ou = r.o[u] - p.a[u], ov = r.o[v] - p.a[v], ow = r.o[w] - p.a[w];
  float du = r.d[u], dv = r.d[v], dw = r.d[w];
  float best = tmax; bool found = false;
  float nu = 0.f, nv = 0.f, nw = 0.f;
  float A = du * du + dv * dv;
  if (A > 1e-9f) {                                   // curved wall
    float B = ou * du + ov * dv, C = ou * ou + ov * ov - R * R;
    float disc = B * B - A * C;
    if (disc >= 0.f) {
      float s = fsqrt(disc);
      float ts[2] = {(-B - s) / A, (-B + s) / A};
      for (int k = 0; k < 2; ++k) {
        float t = ts[k];
        if (t > EPS && t < best && fabsx(ow + t * dw) <= H) {
          best = t; found = true; nu = (ou + t * du) / R; nv = (ov + t * dv) / R; nw = 0.f; break;
        }
      }
    }
  }
  if (fabsx(dw) > 1e-9f) {                           // end caps
    for (int k = 0; k < 2; ++k) {
      float cap = k == 0 ? H : -H;
      float t = (cap - ow) / dw;
      if (t > EPS && t < best) {
        float pu = ou + t * du, pv = ov + t * dv;
        bool open = p.pad && k == 0 && inHole(r.o.x + r.d.x * t, r.o.z + r.d.z * t);   // pool opening
        if (!open && pu * pu + pv * pv <= R * R) { best = t; found = true; nu = 0.f; nv = 0.f; nw = k == 0 ? 1.f : -1.f; }
      }
    }
  }
  if (!found) return false;
  tOut = best;
  nOut = axisVec(u, nu) + axisVec(v, nv) + axisVec(w, nw);
  return true;
}

RT_INLINE bool hitPrim(const Prim& p, const Ray& r, float tmax, float& t, V3& n) {
  switch (p.type) {
    case P_BOX:    return hitBox(p, r, tmax, t, n);
    case P_SPHERE: return hitSphere(p, r, tmax, t, n);
    default:       return hitCyl(p, r, tmax, t, n);
  }
}

}  // namespace rt
