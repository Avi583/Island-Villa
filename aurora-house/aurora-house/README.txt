AURORA HOUSE
Real-time ray tracing in hand-written C++, compiled to WebAssembly
=====================================================================

A futuristic glass house on a round island in a lake, with cars circling a
ring road. You walk the perimeter and watch reflections, refraction and
soft shadows update live. Every pixel is computed by the C++ code in src/,
running on all CPU cores in the browser. No WebGL, no game engine, no
third-party libraries.


WHAT THE C++ DOES
---------------------------------------------------------------------
- Freestanding build: no libc, no Emscripten. sin, cos, exp, log, pow and
  atan2 are implemented from scratch (src/mathx.h). The whole renderer is a
  single ~52 KB .wasm file.
- Bounding volume hierarchy built with a binned surface-area heuristic,
  with near-child-first traversal and a separate early-out shadow query
  (src/geometry.h).
- Analytic ray intersection for boxes, spheres and cylinders.
- Cars are instanced: one shared model, intersected in each car's own
  rotated frame after a bounding-sphere test.
- Water: refraction (index 1.333), Fresnel reflection, depth absorption
  (Beer-Lambert) and animated waves from a sum of directional sine waves
  with analytic normals. Used for both the lake and the rooftop pool.
- Thin architectural glass, clear-coat car paint, brushed and mirror metals.
- Soft sun shadows from an area sun, one bounce of global illumination,
  point lights for neon, lamps and headlights.
- Sky model with a sun halo, procedural mountain horizon and stars at
  night; distance haze; ACES tone mapping.
- Progressive accumulation: the image keeps refining while you stand still.
- Multi-threading: the browser starts one Web Worker per CPU core, each
  running its own copy of the module and rendering every Nth row so the
  work is evenly balanced.


CONTROLS
---------------------------------------------------------------------
Click the view        Capture the mouse to look around
W A S D / arrows      Walk
Shift                 Run
T                     Automatic perimeter tour
V                     Drone view (fly above the house)
P                     Photo mode (full resolution, converges to a clean image)
Space                 Freeze time
F                     Fullscreen
H                     Hide the interface
Touch                 Left half of the screen moves, right half looks

The settings panel has time-of-day presets (Golden, Noon, Dusk, Night), sun
elevation and direction, exposure, quality level, render scale, and toggles
for shadows, bloom and freezing time. Stand still for a second or two and
the image sharpens as samples accumulate.


FILES
---------------------------------------------------------------------
src/mathx.h        Freestanding math, vectors, random numbers, sampling
src/geometry.h     Rays, primitives, intersection, SAH BVH
src/scene.h        Material, light and scene declarations
src/scene.cpp      The house, island, dock, trees, lamps and animated cars
src/render.cpp     Sky, textures, water, lighting, tracer, tone mapping,
                   and the functions the browser calls
docs/index.html    The page and interface
docs/main.js       Input, camera, collision, worker pool, display
docs/worker.js     Loads the WebAssembly module and renders its rows
docs/raytracer.wasm  The compiled C++ (already built, ready to serve)
docs/.nojekyll     Tells GitHub Pages to serve the files as-is
build.sh           Rebuilds docs/raytracer.wasm from src/
To-Do-List.txt     Known issues and untested items


PUTTING IT ON GITHUB
---------------------------------------------------------------------
Upload the whole folder to a repository, keeping the folder structure.
The site itself is everything in docs/; the .wasm file is already built,
so nothing needs compiling to host it. Point GitHub Pages at the docs/
folder of your main branch.

docs/raytracer.wasm is a binary file. Upload it as a file (drag and drop
in GitHub's "Upload files" page); it cannot be copy-pasted as text.


REBUILDING AFTER CHANGING THE C++
---------------------------------------------------------------------
Requires clang 15 or newer with the wasm32 target and wasm-ld (lld).
On Ubuntu/Debian:  sudo apt install clang lld
On macOS:          brew install llvm   (Apple's clang lacks wasm-ld)

Then run:
    ./build.sh

This writes a new docs/raytracer.wasm. Commit it along with your changes.


RUNNING LOCALLY
---------------------------------------------------------------------
Browsers will not load the .wasm or workers from a file:// URL, so serve
the docs folder over HTTP:
    cd docs
    python3 -m http.server 8000
Then open http://localhost:8000


BROWSER REQUIREMENTS
---------------------------------------------------------------------
A current desktop browser with WebAssembly SIMD: Chrome/Edge 91+,
Firefox 89+, Safari 16.4+. More CPU cores means higher resolution and frame
rate; the render scale adjusts automatically to keep it responsive.
Only Chromium has been tested so far (see To-Do-List.txt).
