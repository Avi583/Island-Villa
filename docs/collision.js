// collision.js — walkable-area data (tree, pillar and lamp circles, house and pool boxes, island edge,
// boardwalk) and the collide() function that pushes the player position out of them.
const DEG = Math.PI / 180;

export const CIRCLES = [[-9.5, 7, 0.45], [6, 7, 0.45], [6, -6.6, 0.45]];
for (let k = 0; k < 6; k++) { const a = (30 + 60 * k) * DEG; CIRCLES.push([15.5 * Math.cos(a), 15.5 * Math.sin(a), 0.55]); }
for (let k = 0; k < 8; k++) { const a = (22.5 + 45 * k) * DEG; CIRCLES.push([13.8 * Math.cos(a), 13.8 * Math.sin(a), 0.3]); }
export const HOUSE = { x0: -8.45, x1: 8.3, z0: -6.45, z1: 6.3 };   // glass walls + body radius

export const POOL = { x0: -7.9, x1: 7.9, z0: 8.5, z1: 12.1 };      // water edge + body radius
export function collide(p, prev, drone) {
  if (drone) {
    const r = Math.hypot(p.x, p.z);
    if (r > 70) { p.x *= 70 / r; p.z *= 70 / r; }
    return;
  }
  for (const [cx, cz, r] of CIRCLES) {
    const dx = p.x - cx, dz = p.z - cz, d = Math.hypot(dx, dz);
    if (d < r && d > 1e-6) { p.x = cx + dx / d * r; p.z = cz + dz / d * r; }
  }
  if (p.x > POOL.x0 && p.x < POOL.x1 && p.z > POOL.z0 && p.z < POOL.z1) {            // don't walk into the pool
    const pen = [p.x - POOL.x0, POOL.x1 - p.x, p.z - POOL.z0, POOL.z1 - p.z];
    const i = pen.indexOf(Math.min(...pen));
    if (i === 0) p.x = POOL.x0; else if (i === 1) p.x = POOL.x1; else if (i === 2) p.z = POOL.z0; else p.z = POOL.z1;
  }
  if (p.x > HOUSE.x0 && p.x < HOUSE.x1 && p.z > HOUSE.z0 && p.z < HOUSE.z1) {
    const pen = [p.x - HOUSE.x0, HOUSE.x1 - p.x, p.z - HOUSE.z0, HOUSE.z1 - p.z];
    const i = pen.indexOf(Math.min(...pen));
    if (i === 0) p.x = HOUSE.x0; else if (i === 1) p.x = HOUSE.x1; else if (i === 2) p.z = HOUSE.z0; else p.z = HOUSE.z1;
  }
  const prevOnDock = Math.hypot(prev.x, prev.z) > 23.35;
  const r = Math.hypot(p.x, p.z);
  if (prevOnDock || (r > 23.3 && p.z > 0 && Math.abs(p.x) < 1.2)) {       // boardwalk
    p.x = Math.max(-1.2, Math.min(1.2, p.x));
    p.z = Math.min(33.4, p.z);
    if (Math.hypot(p.x, p.z) < 23.3 && !prevOnDock) return;
  } else if (r > 23.3) { p.x *= 23.3 / r; p.z *= 23.3 / r; }
}
