#!/usr/bin/env bash
# Builds the C++ ray tracer into docs/raytracer.wasm (the folder GitHub Pages serves).
# Requires only clang >= 15 with the wasm32 target and wasm-ld (lld). No Emscripten.
set -euo pipefail
cd "$(dirname "$0")"

CXX="${CXX:-clang++}"
mkdir -p docs

"$CXX" --target=wasm32 -std=c++17 -O3 \
  -nostdlib -ffreestanding -fno-exceptions -fno-rtti \
  -fno-math-errno -fno-trapping-math -ffp-contract=fast \
  -msimd128 -mbulk-memory -mnontrapping-fptoint \
  -Wall -Wextra -Wno-unused-function -Wno-unused-parameter \
  -Wl,--no-entry -Wl,--export-dynamic -Wl,--strip-all \
  -Wl,-z,stack-size=1048576 \
  -o docs/raytracer.wasm \
  src/render.cpp src/scene.cpp

touch docs/.nojekyll
echo "Built docs/raytracer.wasm ($(wc -c < docs/raytracer.wasm) bytes)"
