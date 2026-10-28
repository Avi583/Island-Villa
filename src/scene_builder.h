#pragma once
// scene_builder.h — helpers shared by the scene-building code: the scene BVH and short
// primitive/light adders (B = box, S = sphere, C = cylinder, L = point light).
#include "scene.h"
#include "bvh.h"

namespace rt {

inline BVH<1024> g_bvh;

inline void B(float x0, float y0, float z0, float x1, float y1, float z1, i32 mat) {
  g_bvh.add(mkBox(V3(x0, y0, z0), V3(x1, y1, z1), mat));
}
inline void S(float x, float y, float z, float r, i32 mat) { g_bvh.add(mkSphere(V3(x, y, z), r, mat)); }
inline void C(float x, float y, float z, float r, float hh, i32 axis, i32 mat) {
  g_bvh.add(mkCyl(V3(x, y, z), r, hh, axis, mat));
}
inline void L(float x, float y, float z, V3 col, float range) {
  if (g_lightCount >= MAX_LIGHTS) return;
  Light& l = g_lights[g_lightCount++];
  l.p = V3(x, y, z); l.col = col; l.range = range;
}

// Defined in scene_props.cpp / car.cpp
void buildTree(float x, float z, u32 seed);
void buildCars();
bool hitCars(const Ray& r, Hit& h, bool any);

}  // namespace rt
