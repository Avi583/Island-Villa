// ─────────────────────────────────────────────────────────────────────────────
// scene.cpp — builds the static world into a SAH BVH, animates the cars, and
// answers closest-hit / any-hit queries for the tracer.
//
// Coordinates: metres, +Y up. The island is a disc of radius 24 m whose top is
// at y = 0.5; the lake surface is the plane y = 0 with a sand bed at y = -3.
// ─────────────────────────────────────────────────────────────────────────────
#include "scene.h"

namespace rt {

Material      g_mats[M_COUNT];
unsigned char g_transmissive[M_COUNT];
Light         g_lights[MAX_LIGHTS];
i32           g_lightCount;

static BVH<1024> g_bvh;
static i32      g_staticLights;

// ── cars: one shared local-space template, instanced with a rotation ─────────
struct Car { V3 pos, f, s; float R, speed, phase, dir; i32 paint; };
static Car  g_cars[8];
static i32  g_carCount;
static Prim g_carParts[24];
static i32  g_carPartCount;
constexpr float CAR_BOUND_R = 2.7f;     // bounding sphere around (pos + 0.8 up)

// ── helpers ──────────────────────────────────────────────────────────────────
static void defMat(i32 id, i32 kind, V3 albedo, float f0 = 0.04f, float rough = 0.f,
                   i32 tex = T_NONE, V3 emit = V3(0.f), V3 sigma = V3(0.f)) {
  Material& m = g_mats[id];
  m.kind = kind; m.tex = tex; m.albedo = albedo; m.emit = emit; m.sigma = sigma;
  m.f0 = f0; m.rough = rough;
  g_transmissive[id] = (kind == K_GLASS || kind == K_WATER) ? 1 : 0;
}
static void B(float x0, float y0, float z0, float x1, float y1, float z1, i32 mat) {
  g_bvh.add(mkBox(V3(x0, y0, z0), V3(x1, y1, z1), mat));
}
static void S(float x, float y, float z, float r, i32 mat) { g_bvh.add(mkSphere(V3(x, y, z), r, mat)); }
static void C(float x, float y, float z, float r, float hh, i32 axis, i32 mat) {
  g_bvh.add(mkCyl(V3(x, y, z), r, hh, axis, mat));
}
static void L(float x, float y, float z, V3 col, float range) {
  if (g_lightCount >= MAX_LIGHTS) return;
  Light& l = g_lights[g_lightCount++];
  l.p = V3(x, y, z); l.col = col; l.range = range;
}
static void carPart(Prim p) { g_carParts[g_carPartCount++] = p; }

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
  defMat(M_POOL,     K_WATER,   V3(1.f),                      0.02f, 0.f, T_NONE, V3(0.f), V3(0.30f, 0.055f, 0.035f));
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

  // Outdoor pool at floor level: a deck continuing the ground-floor slab (top y = 0.8),
  // with a tiled floor and a water volume whose sides act as the glass walls
  // Four slabs frame the opening; the tiled floor sits just below the deck and the water is flush with it.
  B(-5.f,  0.5f, 6.f,   5.f,  0.8f, 8.6f,  M_CONCRETE);
  B(-5.f,  0.5f, 12.2f, 5.f,  0.8f, 13.f,  M_CONCRETE);
  B(-5.f,  0.5f, 8.6f,  -3.8f, 0.8f, 12.2f, M_CONCRETE);
  B(3.8f,  0.5f, 8.6f,  5.f,  0.8f, 12.2f, M_CONCRETE);
  B(-3.8f, 0.5f, 8.6f,  3.8f, 0.55f, 12.2f, M_POOLTILE);
  B(-3.8f, 0.55f, 8.6f, 3.8f, 0.8f, 12.2f, M_POOL);
}

// ── trees: leaning tapered trunk, root flare, forked limbs, lumpy two-tone crown ──
static float treeRand(u32& s) {                                 // xorshift → [0,1)
  s ^= s << 13; s ^= s >> 17; s ^= s << 5;
  return (float)(s & 0xFFFFFFu) * (1.f / 16777216.f);
}
static void buildTree(float x, float z, u32 seed) {
  u32 s = seed * 2654435761u + 12345u;
  for (int i = 0; i < 4; ++i) treeRand(s);
  const float base = 0.5f;
  float H    = 3.4f + 1.6f * treeRand(s);                       // trunk height
  float r0   = 0.20f + 0.05f * treeRand(s);                     // base radius
  float lean = 0.12f + 0.22f * treeRand(s), la = TAU * treeRand(s);
  float lx = lean * fcos(la), lz = lean * fsin(la);             // total horizontal drift at the top

  S(x, base + 0.05f, z, r0 * 1.5f, M_TRUNK);                    // root flare
  for (int i = 0; i < 3; ++i) {                                 // buttress roots
    float ra = TAU * (i + treeRand(s)) / 3.f;
    S(x + r0 * 1.5f * fcos(ra), base + 0.04f, z + r0 * 1.5f * fsin(ra), r0 * 0.55f, M_TRUNK);
  }
  const int N = 9;                                              // trunk: overlapping spheres on a curve
  float tx = x, ty = base, tz = z, tr = r0;
  for (int i = 0; i <= N; ++i) {
    float t = (float)i / N, bend = t * t;
    tx = x + lx * bend; tz = z + lz * bend; ty = base + H * t;
    tr = r0 * (1.f - 0.55f * t);
    S(tx, ty, tz, tr, M_TRUNK);
  }
  for (int i = 0; i < 3; ++i) {                                 // limbs from the upper trunk into the crown
    float ba = TAU * (i + 0.3f * treeRand(s)) / 3.f + la;
    float bx = fcos(ba), bz = fsin(ba), len = 0.9f + 0.6f * treeRand(s);
    for (int j = 1; j <= 4; ++j) {
      float u = (float)j / 4;
      S(tx + bx * len * u, ty - 0.5f + len * u * 0.9f, tz + bz * len * u,
        tr * (0.75f - 0.45f * u), M_TRUNK);
    }
  }

  // crown: central mass plus lobes on a shell, bigger lobes low and outward, small ones on top
  float cr = 1.3f + 0.35f * treeRand(s), cy = ty + cr * 0.55f;
  S(tx, cy, tz, cr, M_FOLIAGE);
  const int LOBES = 11;
  for (int i = 0; i < LOBES; ++i) {
    float ang = TAU * (i + treeRand(s)) / LOBES;
    float elev = -0.15f + 1.0f * treeRand(s);                   // −0.15 … 0.85 rad-ish up the shell
    float rr = cr * (0.5f + 0.25f * treeRand(s)) * (1.f - 0.3f * elev);
    float d = cr * (0.75f + 0.2f * treeRand(s)), h = fsqrt(fmaxx(0.f, 1.f - elev * elev));
    S(tx + d * h * fcos(ang), cy + d * elev, tz + d * h * fsin(ang), rr,
      treeRand(s) < 0.4f ? M_FOLIAGE2 : M_FOLIAGE);
  }
  for (int i = 0; i < 4; ++i) {                                 // sparse leaf clumps hanging below the canopy edge
    float ang = TAU * treeRand(s), d = cr * (1.0f + 0.3f * treeRand(s));
    S(tx + d * fcos(ang), cy - cr * (0.1f + 0.3f * treeRand(s)), tz + d * fsin(ang),
      cr * (0.25f + 0.12f * treeRand(s)), M_FOLIAGE);
  }
}

// ── island landscaping, dock ─────────────────────────────────────────────────
static void buildGrounds() {
  C(0.f, -1.5f, 0.f, ISLAND_R, 2.0f, 1, M_ISLAND);          // island disc: y -3.5 … 0.5

  for (int k = 0; k < 6; ++k) {                             // trees between path and road
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
  L(0.f, 1.1f, 10.4f, V3(0.3f, 1.3f, 1.7f), 4.5f);          // pool light
  L(-5.f, 8.35f, 0.f, V3(2.5f, 1.8f, 1.0f), 5.f);           // dome orb
}

// ── car template (local frame: +x forward, +y up, wheels touch y = 0) ────────
static void buildCarTemplate() {
  g_carPartCount = 0;
  carPart(mkBox(V3(-2.2f, 0.30f, -0.95f), V3(2.2f, 0.85f, 0.95f), M_CARPAINT));     // body
  carPart(mkBox(V3(0.9f, 0.85f, -0.9f),  V3(2.15f, 0.93f, 0.9f), M_CARPAINT));      // hood
  carPart(mkBox(V3(-1.3f, 0.85f, -0.82f), V3(0.9f, 1.36f, 0.82f), M_CARGLASS));     // glass canopy
  carPart(mkBox(V3(-1.1f, 1.36f, -0.7f), V3(0.6f, 1.40f, 0.7f), M_CARPAINT));       // roof strip
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

static void addCar(float R, float speed, float phase, float dir, i32 paint) {
  Car& c = g_cars[g_carCount++];
  c.R = R; c.speed = speed; c.phase = phase; c.dir = dir; c.paint = paint;
  c.pos = V3(0.f); c.f = V3(1.f, 0.f, 0.f); c.s = V3(0.f, 0.f, 1.f);
}

void buildScene() {
  g_bvh.clear();
  g_lightCount = 0;
  g_carCount = 0;
  buildMaterials();
  buildHouse();
  buildGrounds();
  buildLights();
  g_staticLights = g_lightCount;
  buildCarTemplate();
  addCar(18.6f,  9.f, 0.f,        1.f, M_PAINT0);   // inner lane, anticlockwise
  g_bvh.build();
  updateDynamic(0.f);
}

void updateDynamic(float time) {
  g_lightCount = g_staticLights;
  for (i32 i = 0; i < g_carCount; ++i) {
    Car& c = g_cars[i];
    float phi = c.phase + c.dir * (c.speed / c.R) * time;
    float sp = fsin(phi), cp = fcos(phi);
    c.pos = V3(c.R * cp, ISLAND_TOP, c.R * sp);
    c.f = V3(-sp * c.dir, 0.f, cp * c.dir);          // tangent of travel
    c.s = V3(-c.f.z, 0.f, c.f.x);
    if (g_lightCount < MAX_LIGHTS) {                  // headlight pool on the road ahead
      Light& l = g_lights[g_lightCount++];
      l.p = c.pos + c.f * 4.f + V3(0.f, 0.9f, 0.f);
      l.col = V3(2.4f, 2.2f, 1.8f); l.range = 8.f;
    }
  }
}

// ── queries ──────────────────────────────────────────────────────────────────
// Instanced car test: cheap world-space bounding sphere, then the template in the car's frame.
static bool hitCars(const Ray& r, Hit& h, bool any) {
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

bool intersectScene(const Ray& r, float tmax, Hit& h) {
  h.t = tmax; h.obj = -1; h.mat = 0; h.n = V3(0.f, 1.f, 0.f);
  bool hit = g_bvh.traverse<false>(r, h, nullptr);
  if (r.d.y != 0.f) {                                 // infinite lake surface and bed
    float t = (LAKE_Y - r.o.y) / r.d.y;
    if (t > EPS && t < h.t) { h.t = t; h.n = V3(0.f, 1.f, 0.f); h.mat = M_LAKE; h.obj = -1; hit = true; }
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
