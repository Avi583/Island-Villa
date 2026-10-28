# Directory

Every file in the project, with a short description. Keep this updated whenever a file is added, removed, or moved.

## Root

| Path | Description |
|------|-------------|
| [CLAUDE.md](CLAUDE.md) | Instructions for Claude: workflow rules, directory file and to-do list conventions. |
| [DIRECTORY.md](DIRECTORY.md) | This file: index of every file in the project. |
| [LICENSE](LICENSE) | Project license. |
| [README.txt](README.txt) | Project overview, features, controls, build and run instructions. |
| [To-Do-List.txt](To-Do-List.txt) | Known issues, untested items, and planned ideas. |
| [build.sh](build.sh) | Compiles `src/` to `docs/raytracer.wasm` with clang (freestanding wasm32, no Emscripten). |

## src/ (C++ ray tracer source)

| Path | Description |
|------|-------------|
| [src/mathx.h](src/mathx.h) | Freestanding math (sin, cos, exp, etc.), vectors, 4-wide SIMD types, random numbers, sampling. |
| [src/geometry.h](src/geometry.h) | Umbrella header that includes `primitives.h` and `bvh.h`. |
| [src/primitives.h](src/primitives.h) | Rays, hit records, analytic primitives (box, sphere, cylinder), pool-opening rectangle (`g_hole`). |
| [src/bvh.h](src/bvh.h) | Binned-SAH BVH collapsed to a SIMD 4-wide BVH. |
| [src/scene.h](src/scene.h) | Material, light and scene declarations. |
| [src/scene.cpp](src/scene.cpp) | Materials, house, island, dock and lights; scene hit queries. |
| [src/scene_builder.h](src/scene_builder.h) | Scene BVH (`g_bvh`) and the B/S/C/L builder helpers shared by scene files. |
| [src/scene_props.cpp](src/scene_props.cpp) | Procedural palm trees. |
| [src/car.cpp](src/car.cpp) | Car template, instancing and ray test. |
| [src/runtime.h](src/runtime.h) | Freestanding memset/memcpy/memmove and the heap allocator. |
| [src/frame.h](src/frame.h) | Per-frame state shared by sky, water and lighting. |
| [src/sky.h](src/sky.h) | Sky gradient, sun halo, stars, mountains, haze, ambient light. |
| [src/texture.h](src/texture.h) | Value noise and procedural albedo textures. |
| [src/water.h](src/water.h) | Sine-wave water normals. |
| [src/lighting.h](src/lighting.h) | Direct light, emission, one-bounce GI. |
| [src/tonemap.h](src/tonemap.h) | ACES tone mapping, gamma LUT, sRGB output. |
| [src/render.cpp](src/render.cpp) | Tracer, reflection shading, frame setup and the exported WebAssembly functions. |

## docs/ (site served by GitHub Pages)

| Path | Description |
|------|-------------|
| [docs/index.html](docs/index.html) | The page and user interface. |
| [docs/main.js](docs/main.js) | Entry point (ES module): the requestAnimationFrame loop and startup. |
| [docs/state.js](docs/state.js) | Shared state: camera/scene state, quality modes, presets, thread count, `pipe` (shared frame-pipeline values). |
| [docs/pipeline.js](docs/pipeline.js) | Worker pool and frame pipeline: boot, dispatch, row collection, auto-resolution, present. |
| [docs/gpu.js](docs/gpu.js) | WebGL present path: temporal reprojection, denoise, bloom, upscale. |
| [docs/denoise.js](docs/denoise.js) | CPU bilateral denoise (fallback without WebGL). |
| [docs/input.js](docs/input.js) | Keyboard, mouse and touch input, movement, tour/drone toggles. |
| [docs/ui.js](docs/ui.js) | Settings panel, presets, screenshot, stats, loading screen, resize. |
| [docs/shaders.js](docs/shaders.js) | GLSL shader sources for the WebGL present path. |
| [docs/collision.js](docs/collision.js) | Walkable-area data and `collide()`. |
| [docs/worker.js](docs/worker.js) | Web Worker that loads the WebAssembly module and renders its share of rows. |
| [docs/raytracer.wasm](docs/raytracer.wasm) | Compiled ray tracer (built output of `build.sh`). |
| [docs/.nojekyll](docs/.nojekyll) | Tells GitHub Pages to serve files as-is. |
