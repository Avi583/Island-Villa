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

// ── BVH4 node: four child boxes stored per axis so one SIMD slab test covers all four ──
struct BVH4Node {
  F4 mnx, mny, mnz, mxx, mxy, mxz;
  I4 child;   // inner: BVH4 node index; leaf: first primitive
  I4 cnt;     // > 0 leaf primitive count, 0 inner node, -1 empty slot
};

// Ray (origin o, inverse direction iv, splatted) vs the four child boxes of a node.
// Lane-for-lane the same arithmetic as a scalar slab test, so results match bit for bit.
RT_INLINE I4 hitAABB4(const BVH4Node& nd, F4 ox, F4 oy, F4 oz, F4 ix, F4 iy, F4 iz, float tmax, F4& tnear) {
  F4 tx1 = (nd.mnx - ox) * ix, tx2 = (nd.mxx - ox) * ix;
  F4 t0 = vmin4(tx1, tx2), t1 = vmax4(tx1, tx2);
  F4 ty1 = (nd.mny - oy) * iy, ty2 = (nd.mxy - oy) * iy;
  t0 = vmax4(t0, vmin4(ty1, ty2)); t1 = vmin4(t1, vmax4(ty1, ty2));
  F4 tz1 = (nd.mnz - oz) * iz, tz2 = (nd.mxz - oz) * iz;
  t0 = vmax4(t0, vmin4(tz1, tz2)); t1 = vmin4(t1, vmax4(tz1, tz2));
  tnear = t0;
  return (t1 >= vmax4(t0, splat4(0.f))) & (t0 < splat4(tmax)) & (nd.cnt >= 0);
}

RT_INLINE float halfArea(V3 mn, V3 mx) {
  V3 e = mx - mn;
  return (e.x < 0.f) ? 0.f : e.x * e.y + e.y * e.z + e.z * e.x;
}

// ── BVH (binned surface-area heuristic) ──────────────────────────────────────
// Built as a binary tree, then collapsed into a 4-wide tree (BVH4) for traversal.
struct BVHNode { V3 mn; i32 leftFirst; V3 mx; i32 count; };   // count > 0 → leaf

template <int MAXP>
struct BVH {
  Prim     prims[MAXP];       // reordered into leaf order by build()
  i32      idx[MAXP];
  V3       cent[MAXP];
  BVHNode  nodes[2 * MAXP];   // binary tree (build scratch)
  BVH4Node nodes4[MAXP];
  i32      primCount, nodeCount, node4Count;

  void clear() { primCount = 0; nodeCount = 0; node4Count = 0; }
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
    reorder();
    node4Count = 0;
    if (nodes[0].count) {                             // whole scene in one leaf
      BVH4Node& n4 = nodes4[node4Count++];
      n4.cnt = I4{-1, -1, -1, -1};
      setLane(n4, 0, 0);
    } else {
      collapse(0);
    }
  }

  // Permute prims[] into idx[] order (cycle by cycle, in place) so leaves read contiguous memory.
  void reorder() {
    for (i32 i = 0; i < primCount; ++i) {
      Prim t = prims[i];
      i32 j = i;
      for (;;) {
        i32 k = idx[j]; idx[j] = j;
        if (k == i) break;
        prims[j] = prims[k]; j = k;
      }
      prims[j] = t;
    }
  }

  // Copy binary node bi's box into lane k; leaves point straight at their primitives.
  void setLane(BVH4Node& n4, i32 k, i32 bi) {
    const BVHNode& b = nodes[bi];
    n4.mnx[k] = b.mn.x; n4.mny[k] = b.mn.y; n4.mnz[k] = b.mn.z;
    n4.mxx[k] = b.mx.x; n4.mxy[k] = b.mx.y; n4.mxz[k] = b.mx.z;
    n4.child[k] = b.leftFirst; n4.cnt[k] = b.count;
  }

  // Binary inner node bi → one BVH4 node: open the largest inner child until there are four.
  i32 collapse(i32 bi) {
    i32 ch[4]; i32 n = 2;
    ch[0] = nodes[bi].leftFirst; ch[1] = ch[0] + 1;
    while (n < 4) {
      i32 best = -1; float bestA = -1.f;
      for (i32 k = 0; k < n; ++k) {
        const BVHNode& c = nodes[ch[k]];
        float a = halfArea(c.mn, c.mx);
        if (c.count == 0 && a > bestA) { bestA = a; best = k; }
      }
      if (best < 0) break;
      i32 c = nodes[ch[best]].leftFirst;
      ch[best] = c; ch[n++] = c + 1;
    }
    i32 ni = node4Count++;
    BVH4Node& n4 = nodes4[ni];
    n4.mnx = n4.mny = n4.mnz = n4.mxx = n4.mxy = n4.mxz = splat4(0.f);
    n4.child = I4{0, 0, 0, 0}; n4.cnt = I4{-1, -1, -1, -1};
    for (i32 k = 0; k < n; ++k) {
      setLane(n4, k, ch[k]);
      if (nodes[ch[k]].count == 0) { i32 c = collapse(ch[k]); nodes4[ni].child[k] = c; }
    }
    return ni;
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
    const F4 ox = splat4(r.o.x), oy = splat4(r.o.y), oz = splat4(r.o.z);
    const F4 ix = splat4(r.inv.x), iy = splat4(r.inv.y), iz = splat4(r.inv.z);
    struct Entry { float t; i32 child, cnt; };        // a child box that passed its slab test
    Entry stack[96]; i32 sp = 0;
    stack[sp++] = Entry{-INF, 0, 0};
    bool found = false;
    while (sp) {
      const Entry e = stack[--sp];
      if (e.t >= h.t) continue;                       // a closer hit was found since it was pushed
      if (e.cnt) {
        for (i32 k = 0; k < e.cnt; ++k) {
          const Prim& p = prims[e.child + k];
          if (ANY && skip[p.mat]) continue;
          float t; V3 n;
          if (hitPrim(p, r, h.t, t, n)) {
            h.t = t; h.n = n; h.mat = p.mat; h.obj = -1; found = true;
            if (ANY) return true;
          }
        }
        continue;
      }
      const BVH4Node& nd = nodes4[e.child];
      F4 tn;
      I4 m = hitAABB4(nd, ox, oy, oz, ix, iy, iz, h.t, tn);
      Entry hit[4]; i32 nh = 0;
      for (i32 k = 0; k < 4; ++k) {
        if (!m[k]) continue;
        Entry c{tn[k], nd.child[k], nd.cnt[k]};
        i32 j = nh++;
        if (!ANY) for (; j > 0 && hit[j - 1].t < c.t; --j) hit[j] = hit[j - 1];   // sort far → near
        hit[j] = c;
      }
      for (i32 k = 0; k < nh; ++k) stack[sp++] = hit[k];   // nearest ends up on top
    }
    return found;
  }
};

}  // namespace rt
