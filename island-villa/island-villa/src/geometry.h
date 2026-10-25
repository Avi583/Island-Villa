#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// geometry.h — rays, analytic primitives and a binned-SAH bounding volume
// hierarchy. All storage is fixed-size and static: no heap, no STL.
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

RT_INLINE Prim mkBox(V3 mn, V3 mx, i32 mat) { Prim p; p.a = mn; p.b = mx; p.type = P_BOX; p.axis = 0; p.mat = mat; p.pad = 0; return p; }
RT_INLINE Prim mkSphere(V3 c, float r, i32 mat) { Prim p; p.a = c; p.b = V3(r, 0.f, 0.f); p.type = P_SPHERE; p.axis = 0; p.mat = mat; p.pad = 0; return p; }
RT_INLINE Prim mkCyl(V3 c, float r, float halfH, i32 axis, i32 mat) { Prim p; p.a = c; p.b = V3(r, halfH, 0.f); p.type = P_CYL; p.axis = axis; p.mat = mat; p.pad = 0; return p; }

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
        if (pu * pu + pv * pv <= R * R) { best = t; found = true; nu = 0.f; nv = 0.f; nw = k == 0 ? 1.f : -1.f; }
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

// Ray vs AABB slab test used by BVH traversal.
RT_INLINE bool hitAABB(const Ray& r, V3 mn, V3 mx, float tmax, float& tnear) {
  float tx1 = (mn.x - r.o.x) * r.inv.x, tx2 = (mx.x - r.o.x) * r.inv.x;
  float t0 = fminx(tx1, tx2), t1 = fmaxx(tx1, tx2);
  float ty1 = (mn.y - r.o.y) * r.inv.y, ty2 = (mx.y - r.o.y) * r.inv.y;
  t0 = fmaxx(t0, fminx(ty1, ty2)); t1 = fminx(t1, fmaxx(ty1, ty2));
  float tz1 = (mn.z - r.o.z) * r.inv.z, tz2 = (mx.z - r.o.z) * r.inv.z;
  t0 = fmaxx(t0, fminx(tz1, tz2)); t1 = fminx(t1, fmaxx(tz1, tz2));
  tnear = t0;
  return t1 >= fmaxx(t0, 0.f) && t0 < tmax;
}

RT_INLINE float halfArea(V3 mn, V3 mx) {
  V3 e = mx - mn;
  return (e.x < 0.f) ? 0.f : e.x * e.y + e.y * e.z + e.z * e.x;
}

// ── BVH (binned surface-area heuristic) ──────────────────────────────────────
struct BVHNode { V3 mn; i32 leftFirst; V3 mx; i32 count; };   // count > 0 → leaf

template <int MAXP>
struct BVH {
  Prim    prims[MAXP];
  i32     idx[MAXP];
  V3      cent[MAXP];
  BVHNode nodes[2 * MAXP];
  i32     primCount, nodeCount;

  void clear() { primCount = 0; nodeCount = 0; }
  i32 add(const Prim& p) { if (primCount >= MAXP) return -1; prims[primCount] = p; return primCount++; }

  void build() {
    for (i32 i = 0; i < primCount; ++i) {
      idx[i] = i;
      V3 mn, mx; primBounds(prims[i], mn, mx);
      cent[i] = (mn + mx) * 0.5f;
    }
    nodeCount = 1;
    nodes[0].leftFirst = 0; nodes[0].count = primCount;
    fitBounds(0);
    subdivide(0);
  }

  void fitBounds(i32 ni) {
    BVHNode& nd = nodes[ni];
    V3 mn(INF), mx(-INF);
    for (i32 k = 0; k < nd.count; ++k) {
      V3 a, b; primBounds(prims[idx[nd.leftFirst + k]], a, b);
      mn = vmin(mn, a); mx = vmax(mx, b);
    }
    nd.mn = mn; nd.mx = mx;
  }

  void subdivide(i32 ni) {
    BVHNode& nd = nodes[ni];
    if (nd.count <= 2) return;
    const i32 first = nd.leftFirst, count = nd.count;

    V3 cmn(INF), cmx(-INF);
    for (i32 k = 0; k < count; ++k) { V3 c = cent[idx[first + k]]; cmn = vmin(cmn, c); cmx = vmax(cmx, c); }

    enum { NB = 12 };
    float bestCost = INF; i32 bestAxis = -1, bestBin = 0; float bestScale = 0.f, bestMin = 0.f;
    for (i32 a = 0; a < 3; ++a) {
      float lo = cmn[a], ext = cmx[a] - lo;
      if (ext < 1e-5f) continue;
      float scale = (float)NB / ext;
      V3 bmn[NB], bmx[NB]; i32 cnt[NB];
      for (i32 b = 0; b < NB; ++b) { bmn[b] = V3(INF); bmx[b] = V3(-INF); cnt[b] = 0; }
      for (i32 k = 0; k < count; ++k) {
        i32 pi = idx[first + k];
        i32 b = (i32)((cent[pi][a] - lo) * scale); if (b > NB - 1) b = NB - 1; if (b < 0) b = 0;
        V3 pa, pb; primBounds(prims[pi], pa, pb);
        cnt[b]++; bmn[b] = vmin(bmn[b], pa); bmx[b] = vmax(bmx[b], pb);
      }
      float leftA[NB - 1], rightA[NB - 1]; i32 leftN[NB - 1], rightN[NB - 1];
      V3 lmn(INF), lmx(-INF); i32 ln = 0;
      for (i32 b = 0; b < NB - 1; ++b) {
        ln += cnt[b]; if (cnt[b]) { lmn = vmin(lmn, bmn[b]); lmx = vmax(lmx, bmx[b]); }
        leftN[b] = ln; leftA[b] = ln ? halfArea(lmn, lmx) : 0.f;
      }
      V3 rmn(INF), rmx(-INF); i32 rn = 0;
      for (i32 b = NB - 1; b > 0; --b) {
        rn += cnt[b]; if (cnt[b]) { rmn = vmin(rmn, bmn[b]); rmx = vmax(rmx, bmx[b]); }
        rightN[b - 1] = rn; rightA[b - 1] = rn ? halfArea(rmn, rmx) : 0.f;
      }
      for (i32 b = 0; b < NB - 1; ++b) {
        float cost = (float)leftN[b] * leftA[b] + (float)rightN[b] * rightA[b];
        if (cost < bestCost) { bestCost = cost; bestAxis = a; bestBin = b; bestScale = scale; bestMin = lo; }
      }
    }
    float parentCost = (float)count * halfArea(nd.mn, nd.mx);
    if (bestAxis < 0 || bestCost >= parentCost) return;

    i32 i = first, j = first + count - 1;
    while (i <= j) {
      i32 b = (i32)((cent[idx[i]][bestAxis] - bestMin) * bestScale); if (b > NB - 1) b = NB - 1; if (b < 0) b = 0;
      if (b <= bestBin) ++i;
      else { i32 tmp = idx[i]; idx[i] = idx[j]; idx[j] = tmp; --j; }
    }
    i32 leftCount = i - first;
    if (leftCount == 0 || leftCount == count) return;

    i32 l = nodeCount; nodeCount += 2;
    nodes[l].leftFirst = first;              nodes[l].count = leftCount;
    nodes[l + 1].leftFirst = i;              nodes[l + 1].count = count - leftCount;
    nd.leftFirst = l; nd.count = 0;
    fitBounds(l); fitBounds(l + 1);
    subdivide(l); subdivide(l + 1);
  }

  // ANY = shadow query: early-out on the first opaque hit, skipping materials flagged in `skip`.
  template <bool ANY>
  bool traverse(const Ray& r, Hit& h, const unsigned char* skip) const {
    if (primCount == 0) return false;
    i32 stack[64]; i32 sp = 0; stack[sp++] = 0;
    bool found = false;
    while (sp) {
      const BVHNode& nd = nodes[stack[--sp]];
      float tn;
      if (!hitAABB(r, nd.mn, nd.mx, h.t, tn)) continue;
      if (nd.count) {
        for (i32 k = 0; k < nd.count; ++k) {
          const Prim& p = prims[idx[nd.leftFirst + k]];
          if (ANY && skip[p.mat]) continue;
          float t; V3 n;
          if (hitPrim(p, r, h.t, t, n)) {
            h.t = t; h.n = n; h.mat = p.mat; h.obj = -1; found = true;
            if (ANY) return true;
          }
        }
      } else {
        i32 l = nd.leftFirst, rr = l + 1;
        float tl, tr;
        bool hl = hitAABB(r, nodes[l].mn, nodes[l].mx, h.t, tl);
        bool hr = hitAABB(r, nodes[rr].mn, nodes[rr].mx, h.t, tr);
        if (hl && hr) {                               // visit nearer child first
          if (tl < tr) { stack[sp++] = rr; stack[sp++] = l; }
          else         { stack[sp++] = l;  stack[sp++] = rr; }
        } else if (hl) stack[sp++] = l;
        else if (hr)   stack[sp++] = rr;
      }
    }
    return found;
  }
};

}  // namespace rt
