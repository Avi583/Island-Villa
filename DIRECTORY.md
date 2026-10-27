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
| [src/geometry.h](src/geometry.h) | Rays, analytic primitives (box, sphere, cylinder), binned-SAH BVH collapsed to a SIMD 4-wide BVH. |
| [src/scene.h](src/scene.h) | Material, light and scene declarations. |
| [src/scene.cpp](src/scene.cpp) | Builds the house, island, dock, trees, lamps and animated cars; hit queries. |
| [src/render.cpp](src/render.cpp) | Sky, textures, water, lighting, tracer, tone mapping, and the exported WebAssembly functions. |

## docs/ (site served by GitHub Pages)

| Path | Description |
|------|-------------|
| [docs/index.html](docs/index.html) | The page and user interface. |
| [docs/main.js](docs/main.js) | Input, camera, collision, worker pool and display. |
| [docs/worker.js](docs/worker.js) | Web Worker that loads the WebAssembly module and renders its share of rows. |
| [docs/raytracer.wasm](docs/raytracer.wasm) | Compiled ray tracer (built output of `build.sh`). |
| [docs/.nojekyll](docs/.nojekyll) | Tells GitHub Pages to serve files as-is. |
