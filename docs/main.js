// main.js — entry point: the requestAnimationFrame loop that drives the C++/WebAssembly ray
// tracer across a pool of Web Workers. No rendering logic lives here: every pixel is produced
// by the C++ code in ../src. Modules: state.js (shared state), pipeline.js (workers, frames),
// gpu.js / denoise.js (display), input.js (controls, movement), ui.js (panel, stats).
import { $, pipe } from './state.js';
import { boot, dispatch, present } from './pipeline.js';
import { update } from './input.js';
import { wireUI, resize } from './ui.js';

let lastT = 0;
function loop(t) {
  const dt = Math.min(0.1, (t - (lastT || t)) / 1000);
  lastT = t;
  update(dt);
  if (!pipe.busy && !document.hidden) dispatch();             // kick-start; frameDone() keeps workers busy after that
  if (pipe.latest) { present(pipe.latest); pipe.latest = null; }
  requestAnimationFrame(loop);
}

function start() {
  $('loading').classList.add('gone');
  requestAnimationFrame(loop);
}

resize();
wireUI();
boot(start);
