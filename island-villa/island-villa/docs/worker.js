// worker.js — one instance of the C++ ray tracer per CPU core.
// Each worker owns an interleaved set of rows: rowStart, rowStart + rowStep, …
// so expensive (geometry) and cheap (sky) rows are spread evenly across cores.
let ex = null;
let rowStart = 0, rowStep = 1, curW = 0, curH = 0;

onmessage = async (e) => {
  const m = e.data;

  if (m.type === 'init') {
    try {
      const instance = await WebAssembly.instantiate(m.module, {});
      ex = instance.exports;
      ex.rt_init();
      rowStart = m.index;
      rowStep = m.count;
      postMessage({ type: 'ready', index: rowStart });
    } catch (err) {
      postMessage({ type: 'error', message: String(err) });
    }
    return;
  }

  if (m.type === 'frame' && ex) {
    const { w, h } = m;
    const rows = Math.max(0, Math.ceil((h - rowStart) / rowStep));
    const pixels = rows * w;
    if (w !== curW || h !== curH) {
      if (!ex.rt_alloc(pixels)) { postMessage({ type: 'error', message: 'out of memory' }); return; }
      curW = w; curH = h;
    }
    const t0 = performance.now();
    const c = m.cam;
    ex.rt_render(w, h, rowStart, rowStep, m.time,
                 c[0], c[1], c[2], c[3], c[4], m.fov,
                 m.frame, m.blend, m.sunElev, m.sunAz, m.exposure, m.gi, m.shadows ? 1 : 0);
    // Copy out of WASM memory (it may be detached later by a memory.grow) and transfer.
    const out = new Uint32Array(ex.memory.buffer, ex.rt_out(), pixels).slice();
    postMessage({ type: 'rows', index: rowStart, frame: m.frame, w, h, rows, buf: out.buffer,
                  ms: performance.now() - t0 }, [out.buffer]);
  }
};
