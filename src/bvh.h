#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// bvh.h — binned-SAH bounding volume hierarchy collapsed to a SIMD 4-wide BVH.
// ─────────────────────────────────────────────────────────────────────────────
#include "primitives.h"

namespace rt {

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
