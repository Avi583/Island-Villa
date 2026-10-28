// frame.h — per-frame state (sun, sky colours, time) shared by the sky, water and lighting code.
#pragma once
#include "scene.h"

using namespace rt;

struct Frame {
  V3 sunDir, sunCol, zenith, horizon, horizonAway, midSky;
  float time, night, day;
  i32 shadows;
};
static Frame g_f;
