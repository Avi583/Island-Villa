// car.cpp — the parked car: one shared local-space template, instanced with a rotation, and its
// ray test.
#include "scene_builder.h"

namespace rt {

// ── cars: one shared local-space template, instanced with a rotation ─────────
struct Car { V3 pos, f, s; i32 paint; };
static Car  g_cars[8];
static i32  g_carCount;
static Prim g_carParts[24];
static i32  g_carPartCount;
constexpr float CAR_BOUND_R = 2.7f;     // bounding sphere around (pos + 0.8 up)
static void carPart(Prim p) { g_carParts[g_carPartCount++] = p; }

// ── car template (local frame: +x forward, +y up, wheels touch y = 0) ────────
static void buildCarTemplate() {
  g_carPartCount = 0;
  carPart(mkBox(V3(-2.2f, 0.30f, -0.95f), V3(2.2f, 0.85f, 0.95f), M_CARPAINT));     // body
  carPart(mkBox(V3(0.95f, 0.85f, -0.9f), V3(2.15f, 0.92f, 0.9f), M_CARPAINT));      // hood
  carPart(mkBox(V3(-2.15f, 0.85f, -0.9f), V3(-1.5f, 0.97f, 0.9f), M_CARPAINT));     // trunk deck
  // cabin: two glass steps give a raked windscreen and rear window instead of a box
  carPart(mkBox(V3(-1.5f, 0.85f, -0.82f), V3(0.95f, 1.12f, 0.82f), M_CARGLASS));    // lower glass
  carPart(mkBox(V3(-1.15f, 1.12f, -0.76f), V3(0.55f, 1.36f, 0.76f), M_CARGLASS));   // upper glass
  carPart(mkBox(V3(-0.95f, 1.36f, -0.7f), V3(0.3f, 1.41f, 0.7f), M_CARPAINT));      // roof
  for (int i = 0; i < 4; ++i) {
    float x = (i & 1) ? 1.4f : -1.4f, z = (i & 2) ? 0.85f : -0.85f;
    carPart(mkCyl(V3(x, 0.38f, z), 0.38f, 0.16f, 2, M_RUBBER));                     // tyre
    carPart(mkCyl(V3(x, 0.38f, z), 0.24f, 0.175f, 2, M_CHROME));                    // rim
  }
  carPart(mkBox(V3(2.18f, 0.62f, -0.85f), V3(2.24f, 0.74f, -0.45f), M_HEADLIGHT));
  carPart(mkBox(V3(2.18f, 0.62f, 0.45f),  V3(2.24f, 0.74f, 0.85f),  M_HEADLIGHT));
  carPart(mkBox(V3(-2.24f, 0.62f, -0.9f), V3(-2.18f, 0.72f, 0.9f),  M_TAILLIGHT));  // full-width light bar
  carPart(mkBox(V3(-2.0f, 0.50f, 0.95f),  V3(2.0f, 0.53f, 0.97f),   M_NEON_CYAN));  // side neon
  carPart(mkBox(V3(-2.0f, 0.50f, -0.97f), V3(2.0f, 0.53f, -0.95f),  M_NEON_CYAN));
}

// Parks a car at (x, z) on the island top, nose pointing along (fx, fz) (unit length).
static void addCar(float x, float z, float fx, float fz, i32 paint) {
  Car& c = g_cars[g_carCount++];
  c.paint = paint;
  c.pos = V3(x, ISLAND_TOP, z);
  c.f = V3(fx, 0.f, fz);
  c.s = V3(-fz, 0.f, fx);
}

void buildCars() {
  g_carCount = 0;
  buildCarTemplate();
  addCar(14.8f, 0.f, 0.f, 1.f, M_PAINT0);   // parked on the lawn beside the villa, facing the pool end
}

// Instanced car test: cheap world-space bounding sphere, then the template in the car's frame.
bool hitCars(const Ray& r, Hit& h, bool any) {
  bool found = false;
  for (i32 i = 0; i < g_carCount; ++i) {
    const Car& c = g_cars[i];
    V3 oc = r.o - (c.pos + V3(0.f, 0.8f, 0.f));
    float b = dot(oc, r.d), cc = dot(oc, oc) - CAR_BOUND_R * CAR_BOUND_R;
    if (cc > 0.f && b > 0.f) continue;
    float disc = b * b - cc;
    if (disc < 0.f || -b - fsqrt(disc) > h.t) continue;

    V3 rel = r.o - c.pos;
    Ray lr = makeRay(V3(dot(rel, c.f), rel.y, dot(rel, c.s)), V3(dot(r.d, c.f), r.d.y, dot(r.d, c.s)));
    for (i32 k = 0; k < g_carPartCount; ++k) {
      const Prim& p = g_carParts[k];
      float t; V3 n;
      if (hitPrim(p, lr, h.t, t, n)) {
        h.t = t;
        h.n = c.f * n.x + V3(0.f, n.y, 0.f) + c.s * n.z;
        h.mat = p.mat == M_CARPAINT ? c.paint : p.mat;
        h.obj = i;
        found = true;
        if (any) return true;
      }
    }
  }
  return found;
}

}  // namespace rt
