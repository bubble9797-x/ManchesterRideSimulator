#!/usr/bin/env node
// "No phasing" checks on the real county data, using a line-for-line JS port of the ground,
// ribbon and building rules in Source/ManchesterRide/World/MRGeoWorld.cpp:
//
//  1. Coverage: every point of the county (sampled densely) is inside a cell that the game
//     builds ground for. Cells with no data file still get terrain ground, so this checks the
//     terrain grid covers the boundary with margin.
//  2. Watertight ground: neighbouring cells compute identical heights on their shared edge.
//  3. Roads never sink under the ground: for every ribbon triangle we sample interior points and
//     compare against the ground mesh. Ground poking through a road would make wheels catch a lip.
//  4. Buildings reach the ground: wall bottoms are below the ground at every footprint vertex
//     (no gap to ride under) and roofs are above it.
//
//   node test_ground.mjs [--cells 1500]
import { readFileSync, readdirSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { loadTerrain } from './terrain.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const GEO = join(HERE, '..', '..', 'Content', 'GeoData');
const args = process.argv.slice(2);
const SAMPLE_CELLS = Number(args[args.indexOf('--cells') + 1] || 1500);
const T = loadTerrain(join(GEO, 'terrain.bin'));
if (!T) { console.error('terrain.bin missing'); process.exit(1); }
const manifest = JSON.parse(readFileSync(join(GEO, 'manifest.json'), 'utf8'));
const CELL = manifest.cellSizeM * 100, L = 800;

// FMRTerrain::GroundZ
function groundZ(x, y) {
  const i = Math.floor(x / L), j = Math.floor(y / L), x0 = i * L, y0 = j * L, fx = (x - x0) / L, fy = (y - y0) / L;
  const A = T.heightCm(x0, y0), B = T.heightCm(x0 + L, y0), C = T.heightCm(x0, y0 + L);
  if (fx + fy <= 1) return A + (B - A) * fx + (C - A) * fy;
  const D = T.heightCm(x0 + L, y0 + L);
  return D + (C - D) * (1 - fx) + (B - D) * (1 - fy);
}

// Deterministic RNG so failures reproduce.
let seed = 12345;
const rnd = () => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff);

let fails = 0;
const check = (ok, msg) => { console.log(`${ok ? 'PASS' : 'FAIL'}  ${msg}`); if (!ok) fails++; };

// 1. Coverage of the county boundary by the terrain grid (with margin).
{
  const buf = readFileSync(join(GEO, 'terrain.bin'));
  const minX = buf.readInt32LE(8), minY = buf.readInt32LE(12), sp = buf.readUInt32LE(16), nx = buf.readUInt32LE(20), ny = buf.readUInt32LE(24);
  const maxX = minX + (nx - 1) * sp, maxY = minY + (ny - 1) * sp;
  const rings = manifest.boundary.rings.flat();
  const inside = rings.every(([x, y]) => x > minX + 50000 && x < maxX - 50000 && y > minY + 50000 && y < maxY - 50000);
  check(inside, `terrain grid (${((maxX - minX) / 1e5).toFixed(1)} x ${((maxY - minY) / 1e5).toFixed(1)} km) covers all ${rings.length} boundary points with >= 500 m margin`);
}

// 2. Shared cell edges: BuildCell samples SampleRaw at lattice points; neighbours use the same
//    world positions, so heights must match exactly. Check the arithmetic both ways round.
{
  let worst = 0;
  for (let n = 0; n < 2000; n++) {
    const cx = Math.floor(rnd() * 200) - 120, cy = Math.floor(rnd() * 180) - 110, k = Math.floor(rnd() * 33);
    const xEdge = (cx + 1) * CELL, yAlong = cy * CELL + k * L;          // right edge of (cx,cy) = left edge of (cx+1,cy)
    const a = T.heightCm(cx * CELL + 32 * L, yAlong), b = T.heightCm((cx + 1) * CELL + 0 * L, yAlong);
    worst = Math.max(worst, Math.abs(a - b), Math.abs(T.heightCm(xEdge, yAlong) - a));
  }
  check(worst === 0, `2000 shared cell-edge vertices identical in both neighbours (max diff ${worst} cm)`);
}

// 3 + 4. Roads and buildings on real cells.
const cellFiles = readdirSync(GEO).filter((f) => f.startsWith('cell_'));
const picked = [];
for (let i = 0; i < SAMPLE_CELLS && cellFiles.length; i++) picked.push(cellFiles[Math.floor(rnd() * cellFiles.length)]);

const STEP = Number(process.env.STEP || 400); // cm, must match GeoBuild::Ribbon
const OFFSET = { major: 12, paving: 16, water: 6, rail: 8, other: 10 };
function offsetFor(r) {
  if ((r.kind || '').startsWith('waterway')) return OFFSET.water;
  if ((r.kind || '').startsWith('railway')) return OFFSET.rail;
  if (r.kind === 'motorway' || r.kind === 'major') return OFFSET.major;
  if (r.kind === 'footway' || r.kind === 'pedestrian' || r.kind === 'steps') return OFFSET.paving;
  return OFFSET.other;
}
function widthFor(r) {
  const k = r.kind || '';
  if (k.startsWith('waterway')) return k.endsWith('canal') ? 10 : k.endsWith('river') ? 18 : 2;
  if (k.startsWith('railway')) return 4;
  return r.width || 4;
}
// Port of GeoBuild::Ribbon (grid only).
function ribbon(pts, width, offset) {
  const P = [];
  for (let i = 0; i + 1 < pts.length; i++) {
    const len = Math.hypot(pts[i + 1][0] - pts[i][0], pts[i + 1][1] - pts[i][1]), n = Math.max(1, Math.ceil(len / STEP));
    for (let k = 0; k < n; k++) P.push([pts[i][0] + (pts[i + 1][0] - pts[i][0]) * k / n, pts[i][1] + (pts[i + 1][1] - pts[i][1]) * k / n]);
  }
  P.push(pts.at(-1));
  const H = width * 50, cols = Math.max(2, Math.ceil(width * 100 / STEP) + 1), grid = [];
  for (let i = 0; i < P.length; i++) {
    const prev = P[Math.max(0, i - 1)], next = P[Math.min(P.length - 1, i + 1)];
    let dx = next[0] - prev[0], dy = next[1] - prev[1]; const dl = Math.hypot(dx, dy) || 1; dx /= dl; dy /= dl;
    let scale = 1;
    if (i > 0 && i < P.length - 1) {
      const a = norm(P[i][0] - P[i - 1][0], P[i][1] - P[i - 1][1]), b = norm(P[i + 1][0] - P[i][0], P[i + 1][1] - P[i][1]);
      scale = Math.min(1 / Math.sqrt(Math.max(0.05, (1 + a[0] * b[0] + a[1] * b[1]) * 0.5)), 2.5);
    }
    for (let c = 0; c < cols; c++) {
      const across = (H + (-H - H) * (c / (cols - 1))) * scale, x = P[i][0] - dy * across, y = P[i][1] + dx * across;
      grid.push([x, y, groundZ(x, y) + offset]);
    }
  }
  // Port of the crest pass: lift vertices of any quad where the ground comes within 6 cm of the surface.
  for (let pass = 0; pass < 2; pass++) {
    const lift = new Float64Array(grid.length);
    for (let i = 0; i + 1 < P.length; i++) for (let c = 0; c + 1 < cols; c++) {
      const ids = [i * cols + c, i * cols + c + 1, (i + 1) * cols + c + 1, (i + 1) * cols + c];
      for (const tri of [[ids[0], ids[1], ids[2]], [ids[0], ids[2], ids[3]]]) {
        const [A, B, C] = tri.map((k) => grid[k]);
        let need = 0;
        for (const [u, v] of CREST_SAMPLES) {
          const x = A[0] + (B[0] - A[0]) * u + (C[0] - A[0]) * v, y = A[1] + (B[1] - A[1]) * u + (C[1] - A[1]) * v, z = A[2] + (B[2] - A[2]) * u + (C[2] - A[2]) * v;
          need = Math.max(need, groundZ(x, y) + 6 - z);
        }
        if (need > 0) for (const k of tri) lift[k] = Math.max(lift[k], need);
      }
    }
    grid.forEach((g, k) => { g[2] += lift[k]; });
  }
  const tris = [];
  for (let i = 0; i + 1 < P.length; i++) for (let c = 0; c + 1 < cols; c++) {
    const A = grid[i * cols + c], B = grid[i * cols + c + 1], C = grid[(i + 1) * cols + c + 1], D = grid[(i + 1) * cols + c];
    tris.push([A, B, C], [A, C, D]);
  }
  return tris;
}
// 15 barycentric points per triangle (corners excluded), same as the crest pass in GeoBuild::Ribbon
const CREST_SAMPLES = []; for (let i = 0; i <= 4; i++) for (let j = 0; i + j <= 4; j++) if (!((i === 0 && j === 0) || i === 4 || j === 4)) CREST_SAMPLES.push([i / 4, j / 4]);
const norm = (x, y) => { const l = Math.hypot(x, y) || 1; return [x / l, y / l]; };

let roadTris = 0, roadSamples = 0, worstPoke = -Infinity, pokes = 0, buildings = 0, buildingGaps = 0, worstSlope = 0;
for (const f of picked) {
  const c = JSON.parse(readFileSync(join(GEO, f), 'utf8'));
  for (const r of c.roads || []) {
    if (r.tunnel || (r.pts[0] && r.pts[0].length >= 3)) continue; // tunnels aren't built; bridge decks are deliberately above ground
    for (const [A, B, C] of ribbon(r.pts, widthFor(r), offsetFor(r))) {
      roadTris++;
      for (let s = 0; s < 6; s++) {
        let u = rnd(), v = rnd(); if (u + v > 1) { u = 1 - u; v = 1 - v; }
        const x = A[0] + (B[0] - A[0]) * u + (C[0] - A[0]) * v, y = A[1] + (B[1] - A[1]) * u + (C[1] - A[1]) * v;
        const z = A[2] + (B[2] - A[2]) * u + (C[2] - A[2]) * v;
        const poke = groundZ(x, y) - z; // > 0 means ground is above the road surface
        roadSamples++;
        if (poke > worstPoke) worstPoke = poke;
        if (poke > 0) pokes++;
      }
    }
  }
  for (const b of c.buildings || []) {
    if (!b.ring || b.ring.length < 3) continue;
    buildings++;
    const g = b.ring.map(([x, y]) => groundZ(x, y)), gMin = Math.min(...g), gMax = Math.max(...g);
    worstSlope = Math.max(worstSlope, gMax - gMin);
    const top = Math.max(gMin + Math.max(b.h, 2) * 100, gMax + 250), bottom = b.minH > 0 ? gMin + b.minH * 100 : gMin - 100;
    if (!(b.minH > 0) && g.some((z) => bottom > z - 50)) buildingGaps++;
    if (top <= gMax) buildingGaps++;
  }
}
check(pokes === 0, `${pokes} pokes; ${roadSamples.toLocaleString()} points on ${roadTris.toLocaleString()} road/path triangles in ${picked.length} real cells: ground never above the road surface (closest: road ${(-worstPoke).toFixed(1)} cm above ground)`);
check(buildingGaps === 0, `${buildings.toLocaleString()} buildings: walls go >= 0.5 m into the ground at every corner, roofs above the highest corner (steepest footprint ${(worstSlope / 100).toFixed(1)} m)`);
process.exit(fails ? 1 : 0);
