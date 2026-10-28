#!/usr/bin/env bash
# Builds the C++ ray tracer into docs/raytracer.wasm (the folder GitHub Pages serves).
# Requires only clang >= 15 with the wasm32 target and wasm-ld (lld). No Emscripten.
set -euo pipefail
cd "$(dirname "$0")"

# Use $CXX if set, else clang++ on PATH, else the default Windows LLVM install.
if [ -z "${CXX:-}" ]; then
  if command -v clang++ >/dev/null 2>&1; then CXX=clang++
  elif [ -x "/c/Program Files/LLVM/bin/clang++.exe" ]; then CXX="/c/Program Files/LLVM/bin/clang++.exe"
  else echo "clang++ not found. Install LLVM or set CXX." >&2; exit 1
  fi
fi
mkdir -p docs

"$CXX" --target=wasm32 -std=c++17 -O3 \
  -nostdlib -ffreestanding -fno-exceptions -fno-rtti \
  -fno-math-errno -fno-trapping-math -ffp-contract=fast \
  -msimd128 -mbulk-memory -mnontrapping-fptoint \
  -Wall -Wextra -Wno-unused-function -Wno-unused-parameter \
  -Wl,--no-entry -Wl,--export-dynamic -Wl,--strip-all \
  -Wl,-z,stack-size=1048576 \
  -o docs/raytracer.wasm \
  src/render.cpp src/scene.cpp src/car.cpp src/scene_props.cpp

touch docs/.nojekyll
echo "Built docs/raytracer.wasm ($(wc -c < docs/raytracer.wasm) bytes)"
