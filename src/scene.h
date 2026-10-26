#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// scene.h — materials, lights and the world: a cantilevered glass house on a
// round island, an outdoor pool, a lake, and cars circling a ring road.
// ─────────────────────────────────────────────────────────────────────────────
#include "geometry.h"

namespace rt {

enum MatKind : i32 {
  K_DIFFUSE,   // Lambert + sun/area lights + sky/GI
  K_COAT,      // diffuse base under a Fresnel clear-coat (car paint, marble, tinted glass)
  K_METAL,     // tinted specular with roughness
  K_GLASS,     // thin architectural glass: Fresnel reflection + straight transmission
  K_WATER,     // dielectric volume (IOR 1.333) with Beer–Lambert absorption + animated waves
  K_EMIT       // neon, lamps, headlights
};

enum TexId : i32 { T_NONE, T_ISLAND, T_WOOD, T_CONCRETE, T_POOL, T_SAND, T_FOLIAGE };

struct Material {
  i32 kind, tex;
  V3 albedo, emit, sigma;   // sigma = absorption coefficient per metre (water)
  float f0, rough;
};

enum MatId : i32 {
  M_ISLAND, M_SAND, M_CONCRETE, M_WHITE, M_WOOD, M_GLASS, M_TINT, M_STEEL, M_CHROME,
  M_LAKE, M_POOL, M_POOLTILE, M_NEON_CYAN, M_NEON_MAGENTA, M_PANEL, M_FOLIAGE, M_TRUNK,
  M_FABRIC, M_MARBLE, M_RUBBER, M_CARGLASS, M_HEADLIGHT, M_TAILLIGHT, M_LAMP,
  M_PAINT0, M_PAINT1, M_PAINT2, M_PAINT3,
  M_CARPAINT,   // placeholder in the car template, swapped for each car's own paint
  M_COUNT
};

struct Light { V3 p, col; float range; };   // unshadowed local point light with smooth cutoff

constexpr i32   MAX_LIGHTS  = 40;
constexpr float ISLAND_R    = 24.f;
constexpr float ISLAND_TOP  = 0.5f;
constexpr float LAKE_Y      = 0.f;
constexpr float LAKE_BOTTOM = -3.f;
constexpr float ROAD_IN     = 17.5f;
constexpr float ROAD_OUT    = 21.5f;

extern Material      g_mats[M_COUNT];
extern unsigned char g_transmissive[M_COUNT];
extern Light         g_lights[MAX_LIGHTS];
extern i32           g_lightCount;

void buildScene();
void updateDynamic(float time);
bool intersectScene(const Ray& r, float tmax, Hit& h);
bool occluded(const Ray& r, float tmax);

}  // namespace rt
