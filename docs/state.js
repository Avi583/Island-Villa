// state.js — shared state and settings: the camera/scene state, quality modes, sun presets,
// thread count, and `pipe`, the frame-pipeline values other modules read (stats, main loop).
export const $ = (id) => document.getElementById(id);
export const canvas = $('view');

export const DEG = Math.PI / 180;
export const EYE = 2.2;                                          // island top (0.5) + 1.7 m
export const START = { x: 9.5, z: 15.5 };

export const state = {
  x: START.x, z: START.z, eye: EYE,
  yaw: Math.atan2(-START.z, -START.x) + 0.12, pitch: 0.07, fov: 70 * DEG,
  drone: false, tour: false, tourAngle: Math.atan2(START.z, START.x),
  time: 0, paused: false,
  sunElev: 7 * DEG, sunAz: 2.3, exposure: 1.0,
  quality: 'balanced', bloom: true, denoise: true, upscale: true, shadows: true,
  scale: 0.45, autoRes: true, minScale: 0.3,                       // minScale: auto-resolution floor
};

export const QUALITY = {
  performance: { target: 24, giStill: 0.0, giMove: 0.0, maxW: 960 },
  balanced:    { target: 36, giStill: 0.7, giMove: 0.0, maxW: 1280 },   // GI ray costs the same at any weight, so none while moving
  photo:       { target: 1e9, giStill: 1.0, giMove: 1.0, maxW: 1920 },
};

export const PRESETS = {
  golden: { elev: 7,   az: 2.3 },
  noon:   { elev: 58,  az: 1.9 },
  blue:   { elev: -4,  az: 2.6 },
  night:  { elev: -14, az: 2.6 },
};

export const threads = Math.max(2, Math.min(16, (navigator.hardwareConcurrency || 4) - 1));   // leave a core for the main thread

// Frame-pipeline values shared across modules (written by pipeline.js unless noted).
export const pipe = {
  rw: 0, rh: 0,                                           // current render size
  latest: null,                                           // newest finished frame, waiting to be shown
  busy: false,                                            // workers are rendering a frame
  accumN: 0,                                              // samples accumulated for the current view (ui.js resets it)
  fps: 0, doneMs: 0,                                      // stats
};
