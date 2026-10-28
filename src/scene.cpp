// ─────────────────────────────────────────────────────────────────────────────
// scene.cpp — builds the static world (house, grounds, lights) into a SAH BVH and
// answers closest-hit / any-hit queries for the tracer. Cars live in car.cpp, trees in
// scene_props.cpp, shared builder helpers in scene_builder.h.
//
// Coordinates: metres, +Y up. The island is a disc of radius 24 m whose top is
// at y = 0.5; the lake surface is the plane y = 0 with a sand bed at y = -3.
// ─────────────────────────────────────────────────────────────────────────────
#include "scene_builder.h"

namespace rt {

Material      g_mats[M_COUNT];
unsigned char g_transmissive[M_COUNT];
Light         g_lights[MAX_LIGHTS];
i32           g_lightCount;

// ── helpers ──────────────────────────────────────────────────────────────────
static void defMat(i32 id, i32 kind, V3 albedo, float f0 = 0.04f, float rough = 0.f,
                   i32 tex = T_NONE, V3 emit = V3(0.f), V3 sigma = V3(0.f)) {
  Material& m = g_mats[id];
  m.kind = kind; m.tex = tex; m.albedo = albedo; m.emit = emit; m.sigma = sigma;
  m.f0 = f0; m.rough = rough;
  g_transmissive[id] = (kind == K_GLASS || kind == K_WATER) ? 1 : 0;
}

// ── materials ────────────────────────────────────────────────────────────────
static void buildMaterials() {
  defMat(M_ISLAND,   K_DIFFUSE, V3(0.5f),                     0.04f, 0.f, T_ISLAND);
  defMat(M_SAND,     K_DIFFUSE, V3(0.58f, 0.5f, 0.36f),       0.04f, 0.f, T_SAND);
  defMat(M_CONCRETE, K_DIFFUSE, V3(0.60f, 0.59f, 0.56f),      0.04f, 0.f, T_CONCRETE);
  defMat(M_WHITE,    K_COAT,    V3(0.86f, 0.86f, 0.84f),      0.025f, 0.f, T_CONCRETE);
  defMat(M_WOOD,     K_DIFFUSE, V3(0.42f, 0.24f, 0.12f),      0.04f, 0.f, T_WOOD);
  defMat(M_GLASS,    K_GLASS,   V3(0.86f, 0.93f, 0.95f),      0.045f);
  defMat(M_TINT,     K_COAT,    V3(0.015f, 0.017f, 0.02f),    0.09f, 0.f, T_NONE, V3(0.42f, 0.27f, 0.14f));
  defMat(M_STEEL,    K_METAL,   V3(0.62f, 0.63f, 0.65f),      1.f,  0.18f);
  defMat(M_CHROME,   K_METAL,   V3(0.92f, 0.92f, 0.94f),      1.f,  0.015f);
  defMat(M_LAKE,     K_WATER,   V3(1.f),                      0.02f, 0.f, T_NONE, V3(0.f), V3(0.45f, 0.13f, 0.10f));
  defMat(M_POOL,     K_WATER,   V3(1.f),                      0.02f, 0.f, T_NONE, V3(0.f), V3(0.42f, 0.07f, 0.04f));
  defMat(M_POOLTILE, K_DIFFUSE, V3(0.55f, 0.82f, 0.86f),      0.04f, 0.f, T_POOL);
  defMat(M_NEON_CYAN,    K_EMIT, V3(0.f), 0.f, 0.f, T_NONE, V3(0.35f, 3.6f, 5.0f));
  defMat(M_NEON_MAGENTA, K_EMIT, V3(0.f), 0.f, 0.f, T_NONE, V3(5.0f, 0.5f, 3.4f));
  defMat(M_PANEL,        K_EMIT, V3(0.f), 0.f, 0.f, T_NONE, V3(3.2f, 2.6f, 1.9f));
  defMat(M_FOLIAGE,  K_DIFFUSE, V3(0.10f, 0.24f, 0.06f),      0.04f, 0.f, T_FOLIAGE);
  defMat(M_FOLIAGE2, K_DIFFUSE, V3(0.20f, 0.30f, 0.07f),      0.04f, 0.f, T_FOLIAGE);
  defMat(M_TRUNK,    K_DIFFUSE, V3(0.26f, 0.19f, 0.13f),      0.04f, 0.f, T_BARK);
  defMat(M_FABRIC,   K_DIFFUSE, V3(0.26f, 0.27f, 0.30f));
  defMat(M_MARBLE,   K_COAT,    V3(0.86f, 0.85f, 0.83f),      0.05f);
  defMat(M_RUBBER,   K_DIFFUSE, V3(0.025f));
  defMat(M_CARGLASS, K_COAT,    V3(0.008f, 0.01f, 0.013f),    0.10f);
  defMat(M_HEADLIGHT,K_EMIT, V3(0.f), 0.f, 0.f, T_NONE, V3(9.f, 8.6f, 7.8f));
  defMat(M_TAILLIGHT,K_EMIT, V3(0.f), 0.f, 0.f, T_NONE, V3(6.f, 0.2f, 0.12f));
  defMat(M_LAMP,     K_EMIT, V3(0.f), 0.f, 0.f, T_NONE, V3(6.f, 4.2f, 2.4f));
  defMat(M_PAINT0,   K_COAT,    V3(0.55f, 0.015f, 0.02f),     0.06f);   // candy red
  defMat(M_PAINT1,   K_COAT,    V3(0.82f, 0.82f, 0.84f),      0.06f);   // pearl white
  defMat(M_PAINT2,   K_COAT,    V3(0.01f, 0.22f, 0.32f),      0.06f);   // deep teal
  defMat(M_PAINT3,   K_COAT,    V3(0.035f, 0.035f, 0.04f),    0.07f);   // graphite
  defMat(M_CARPAINT, K_COAT,    V3(0.5f),                     0.06f);
}

// ── the house ────────────────────────────────────────────────────────────────
static void buildHouse() {
  // Ground floor: slab, wood back wall, concrete side wall, full-height glazing
  B(-8.f, 0.5f, -6.f,   8.f, 0.8f, 6.f,  M_CONCRETE);
  B(-8.f, 0.8f, -6.f,   8.f, 4.0f, -5.6f, M_WOOD);
  B(-8.f, 0.8f, -5.6f, -7.6f, 4.0f, 5.85f, M_CONCRETE);
  B(-7.6f, 0.8f, 5.75f, 7.8f, 4.0f, 5.85f, M_GLASS);
  B(7.75f, 0.8f, -5.6f, 7.85f, 4.0f, 5.75f, M_GLASS);
  for (float x = -4.f; x <= 4.1f; x += 4.f) B(x - 0.04f, 0.8f, 5.72f, x + 0.04f, 4.0f, 5.88f, M_STEEL);
  for (float z = -2.f; z <= 2.1f; z += 4.f) B(7.72f, 0.8f, z - 0.04f, 7.88f, 4.0f, z + 0.04f, M_STEEL);
  B(7.72f, 0.8f, 5.72f, 7.88f, 4.0f, 5.88f, M_STEEL);

  // Interior: light strips, kitchen island, sofa, coffee table, accent neon
  for (float z = -3.f; z <= 3.1f; z += 3.f) B(-6.5f, 3.93f, z - 0.15f, 6.5f, 4.0f, z + 0.15f, M_PANEL);
  B(-5.f, 0.8f, -3.f, -1.5f, 1.75f, -1.8f, M_MARBLE);
  B(1.f, 0.8f, -1.f, 5.f, 1.25f, 0.2f, M_FABRIC);
  B(1.f, 0.8f, -1.45f, 5.f, 1.85f, -1.f, M_FABRIC);
  B(2.f, 0.8f, 0.9f, 4.f, 1.15f, 1.9f, M_MARBLE);
  B(-6.f, 2.5f, -5.62f, 6.f, 2.56f, -5.58f, M_NEON_MAGENTA);

  // First-floor slab, cantilevered over the glazing
  B(-10.f, 4.0f, -7.f, 6.5f, 4.4f, 7.5f, M_WHITE);
  C(-9.5f, 2.25f, 7.0f, 0.16f, 1.75f, 1, M_CHROME);
  C(6.0f,  2.25f, 7.0f, 0.16f, 1.75f, 1, M_CHROME);
  C(6.0f,  2.25f, -6.6f, 0.16f, 1.75f, 1, M_CHROME);

  // Upper volume wrapped in a tinted ribbon window, roof plate with overhang
  B(-9.6f, 4.4f, -6.6f, -0.4f, 7.6f, 6.6f, M_WHITE);
  B(-9.2f, 5.0f, 6.6f,  -0.8f, 7.0f, 6.68f, M_TINT);
  B(-9.2f, 5.0f, -6.68f, -0.8f, 7.0f, -6.6f, M_TINT);
  B(-0.4f, 5.0f, -6.2f, -0.32f, 7.0f, 6.2f, M_TINT);
  B(-9.68f, 5.0f, -6.2f, -9.6f, 7.0f, 6.2f, M_TINT);
  B(-10.2f, 7.6f, -7.2f, 0.2f, 7.9f, 7.2f, M_WHITE);

  // Glass observatory dome with a glowing orb
  S(-5.f, 7.9f, 0.f, 2.0f, M_GLASS);
  S(-5.f, 8.35f, 0.f, 0.28f, M_LAMP);

  // Neon edge lighting
  B(-10.f, 3.94f, 7.38f, 6.5f, 4.0f, 7.5f, M_NEON_CYAN);
  B(6.38f, 3.94f, -7.f, 6.5f, 4.0f, 7.5f, M_NEON_CYAN);
  B(-10.2f, 7.54f, 7.2f, 0.2f, 7.6f, 7.27f, M_NEON_MAGENTA);
  B(0.2f, 7.54f, -7.2f, 0.27f, 7.6f, 7.2f, M_NEON_MAGENTA);

  // Outdoor lap pool running the length of the house. A deck continues the ground-floor slab
  // (top y = 0.8); the pool is sunk POOL_DEPTH below the deck through an opening in the island
  // cap (g_hole), lined with tile walls and floor, and filled with a water volume flush with the deck.
  const float PX = 7.6f, PZ0 = 8.8f, PZ1 = 11.8f;            // water footprint: x -PX…PX, z PZ0…PZ1
  const float FLOOR = 0.8f - POOL_DEPTH;                     // tiled floor top
  g_hole = {-PX, PZ0, PX, PZ1};
  B(-8.6f, 0.5f, 6.f,   8.6f, 0.8f, PZ0 - 0.6f, M_CONCRETE);  // deck: four slabs around the opening
  B(-8.6f, 0.5f, PZ1 + 0.6f, 8.6f, 0.8f, 12.4f, M_CONCRETE);
  B(-8.6f, 0.5f, PZ0 - 0.6f, -PX, 0.8f, PZ1 + 0.6f, M_CONCRETE);
  B(PX,    0.5f, PZ0 - 0.6f, 8.6f, 0.8f, PZ1 + 0.6f, M_CONCRETE);
  B(-PX - 0.2f, FLOOR - 0.1f, PZ0 - 0.2f, PX + 0.2f, FLOOR, PZ1 + 0.2f, M_POOLTILE);               // floor
  B(-PX - 0.2f, FLOOR, PZ0 - 0.2f, -PX, 0.5f, PZ1 + 0.2f, M_POOLTILE);                             // walls
  B(PX,         FLOOR, PZ0 - 0.2f,  PX + 0.2f, 0.5f, PZ1 + 0.2f, M_POOLTILE);
  B(-PX, FLOOR, PZ0 - 0.2f, PX, 0.5f, PZ0, M_POOLTILE);
  B(-PX, FLOOR, PZ1,        PX, 0.5f, PZ1 + 0.2f, M_POOLTILE);
  B(-PX, FLOOR, PZ0, PX, 0.8f, PZ1, M_POOL);
  // Coping: close the 0.4 m strip of bare island cap at each end of the opening, then a raised
  // tile lip (0.08 m, 0.25 m wide) around the water so the edge reads from up close.
  B(-PX, 0.5f, PZ0 - 0.6f, PX, 0.8f, PZ0, M_CONCRETE);
  B(-PX, 0.5f, PZ1,        PX, 0.8f, PZ1 + 0.6f, M_CONCRETE);
  B(-PX - 0.25f, 0.8f, PZ0 - 0.25f, PX + 0.25f, 0.88f, PZ0,          M_POOLTILE);
  B(-PX - 0.25f, 0.8f, PZ1,         PX + 0.25f, 0.88f, PZ1 + 0.25f, M_POOLTILE);
  B(-PX - 0.25f, 0.8f, PZ0, -PX,        0.88f, PZ1, M_POOLTILE);
  B( PX,         0.8f, PZ0,  PX + 0.25f, 0.88f, PZ1, M_POOLTILE);
}

// ── island landscaping, dock ─────────────────────────────────────────────────
static void buildGrounds() {
  C(0.f, -1.5f, 0.f, ISLAND_R, 2.0f, 1, M_ISLAND);          // island disc: y -3.5 … 0.5
  g_bvh.prims[g_bvh.primCount - 1].pad = 1;                 // its top cap is open over the pool

  for (int k = 0; k < 6; ++k) {                             // trees on the lawn
    float a = (30.f + 60.f * k) * (PI / 180.f);
    buildTree(15.5f * fcos(a), 15.5f * fsin(a), 1000u + 77u * (u32)k);
  }
  for (int k = 0; k < 8; ++k) {                             // garden lamps along the walkway
    float a = (22.5f + 45.f * k) * (PI / 180.f);
    float x = 13.8f * fcos(a), z = 13.8f * fsin(a);
    C(x, 1.0f, z, 0.05f, 0.5f, 1, M_STEEL);
    S(x, 1.58f, z, 0.12f, M_LAMP);
    L(x, 1.58f, z, V3(1.8f, 1.2f, 0.6f), 4.5f);
  }
  // Boardwalk dock out into the lake (+Z)
  B(-1.5f, 0.3f, 23.0f, 1.5f, 0.45f, 34.f, M_WOOD);
  for (float z = 26.f; z < 34.f; z += 3.f) {
    C(-1.4f, -1.35f, z, 0.1f, 1.65f, 1, M_WOOD);
    C( 1.4f, -1.35f, z, 0.1f, 1.65f, 1, M_WOOD);
  }
  C(-1.4f, 0.75f, 33.8f, 0.05f, 0.3f, 1, M_STEEL); S(-1.4f, 1.1f, 33.8f, 0.1f, M_LAMP);
  C( 1.4f, 0.75f, 33.8f, 0.05f, 0.3f, 1, M_STEEL); S( 1.4f, 1.1f, 33.8f, 0.1f, M_LAMP);
  L(0.f, 1.1f, 33.8f, V3(1.6f, 1.1f, 0.6f), 5.f);
}

static void buildLights() {
  L(-4.f, 3.4f, -1.f, V3(2.2f, 1.6f, 1.0f), 8.f);           // interior
  L( 3.f, 3.4f, 0.5f, V3(2.2f, 1.6f, 1.0f), 8.f);
  for (float x = -8.f; x <= 2.1f; x += 5.f) L(x, 3.7f, 7.2f, V3(0.25f, 1.7f, 2.3f), 5.f);   // under-slab neon spill
  L(6.2f, 3.7f, 2.f,  V3(0.25f, 1.7f, 2.3f), 5.f);
  L(6.2f, 3.7f, -4.f, V3(0.25f, 1.7f, 2.3f), 5.f);
  L(-5.f, 7.2f, 7.4f, V3(2.2f, 0.25f, 1.5f), 4.f);          // roof magenta
  for (float x = -5.f; x <= 5.1f; x += 5.f) L(x, 0.4f, 10.3f, V3(0.3f, 1.3f, 1.7f), 4.5f);   // pool lights
  L(-5.f, 8.35f, 0.f, V3(2.5f, 1.8f, 1.0f), 5.f);           // dome orb
}

void buildScene() {
  g_bvh.clear();
  g_lightCount = 0;
  buildMaterials();
  buildHouse();
  buildGrounds();
  buildLights();
  buildCars();
  g_bvh.build();
}

// ── queries ──────────────────────────────────────────────────────────────────
bool intersectScene(const Ray& r, float tmax, Hit& h) {
  h.t = tmax; h.obj = -1; h.mat = 0; h.n = V3(0.f, 1.f, 0.f);
  bool hit = g_bvh.traverse<false>(r, h, nullptr);
  if (r.d.y != 0.f) {                                 // infinite lake surface and bed
    float t = (LAKE_Y - r.o.y) / r.d.y;
    if (t > EPS && t < h.t && !inHole(r.o.x + r.d.x * t, r.o.z + r.d.z * t)) {   // not inside the pool
      h.t = t; h.n = V3(0.f, 1.f, 0.f); h.mat = M_LAKE; h.obj = -1; hit = true;
    }
    t = (LAKE_BOTTOM - r.o.y) / r.d.y;
    if (t > EPS && t < h.t) { h.t = t; h.n = V3(0.f, 1.f, 0.f); h.mat = M_SAND; h.obj = -1; hit = true; }
  }
  if (hitCars(r, h, false)) hit = true;
  return hit;
}

bool occluded(const Ray& r, float tmax) {
  Hit h; h.t = tmax; h.obj = -1; h.mat = 0;
  if (g_bvh.traverse<true>(r, h, g_transmissive)) return true;
  return hitCars(r, h, true);
}

}  // namespace rt
