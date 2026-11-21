#!/usr/bin/env node
// Builds Content/GeoData/routes.bin: the road network the game uses for map waypoints (sat-nav).
//
//   node make_routes.mjs
//
// Reads every cell_<x>_<y>.json, keeps the ways you can ride a bike or motorbike on, and splits
// them into edges at junctions (points shared by two or more ways, matched to 20 cm so pieces
// clipped at cell borders join up again). Motorways, footways and steps are left out.
//
// Binary, little-endian, Unreal cm (+X east, +Y south):
//   char[4] 'MRRT', u32 version (1)
//   u32 nodes,  then i32 x, i32 y per node
//   u32 points, then i32 x, i32 y per shape point (each edge's points, endpoints included)
//   u32 edges,  then per edge: u32 a, u32 b, u32 firstPoint, u32 numPoints, f32 lengthCm,
//                              f32 costFactor, u32 flags (bit 0 one-way a->b, bits 8..15 kind)
// kind: 0 major, 1 minor, 2 residential, 3 service, 4 cycleway, 5 track, 6 pedestrian, 7 path

import { readdir, readFile, writeFile } from 'node:fs/promises';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const DIR = join(HERE, '..', '..', 'Content', 'GeoData');

// Cost per metre relative to a main road: routes stay on streets you can drive down. Tracks,
// pedestrian streets and paths cost so much that they are only used for the last stretch to a
// waypoint that is on one. Tunnels are left out (the game does not build them yet).
const KINDS = {
  major: [0, 1.0], minor: [1, 1.0], residential: [2, 1.05], service: [3, 1.6],
  cycleway: [4, 1.3], track: [5, 3.0], pedestrian: [6, 5.0], path: [7, 8.0],
};
const SNAP = 20; // cm

const files = (await readdir(DIR)).filter((f) => /^cell_-?\d+_-?\d+\.json$/.test(f));
const ways = []; // { kind, cost, oneway, pts: Int32Array [x0,y0,...] }
let done = 0;
for (const f of files) {
  const c = JSON.parse(await readFile(join(DIR, f), 'utf8'));
  for (const r of c.roads || []) {
    const k = KINDS[r.kind];
    if (!k || !r.pts || r.pts.length < 2 || r.tunnel) continue;
    const pts = new Int32Array(r.pts.length * 2);
    r.pts.forEach((p, i) => { pts[i * 2] = Math.round(p[0]); pts[i * 2 + 1] = Math.round(p[1]); });
    // One-way rules apply to roads, not to cycleways or paths.
    const oneway = !!r.oneway && k[0] <= 3;
    ways.push({ kind: k[0], cost: k[1], oneway, pts });
  }
  if (++done % 2000 === 0) console.log(`read ${done}/${files.length} cells, ${ways.length} ways`);
}

const key = (x, y) => (Math.round(x / SNAP) + 400000) * 1e6 + (Math.round(y / SNAP) + 400000);
// Pass 1: how many way-points sit on each snapped spot. Ends of every way are always nodes.
const uses = new Map();
for (const w of ways) {
  const n = w.pts.length / 2;
  for (let i = 0; i < n; i++) {
    const k = key(w.pts[i * 2], w.pts[i * 2 + 1]);
    uses.set(k, (uses.get(k) || 0) + (i === 0 || i === n - 1 ? 2 : 1));
  }
}
// Pass 2: nodes and edges.
const nodeOf = new Map();
const nodes = [];
const node = (x, y) => {
  const k = key(x, y);
  let id = nodeOf.get(k);
  if (id === undefined) { id = nodes.length / 2; nodeOf.set(k, id); nodes.push(x, y); }
  return id;
};
const points = [];
const edges = [];
for (const w of ways) {
  const n = w.pts.length / 2;
  let start = 0;
  for (let i = 1; i < n; i++) {
    const x = w.pts[i * 2], y = w.pts[i * 2 + 1];
    if (i < n - 1 && (uses.get(key(x, y)) || 0) < 2) continue;
    const a = node(w.pts[start * 2], w.pts[start * 2 + 1]);
    const b = node(x, y);
    let len = 0;
    const first = points.length / 2;
    for (let j = start; j <= i; j++) {
      points.push(w.pts[j * 2], w.pts[j * 2 + 1]);
      if (j > start) len += Math.hypot(w.pts[j * 2] - w.pts[j * 2 - 2], w.pts[j * 2 + 1] - w.pts[j * 2 - 1]);
    }
    if (a !== b && len > 0) edges.push([a, b, first, i - start + 1, len, w.cost, (w.oneway ? 1 : 0) | (w.kind << 8)]);
    else points.length = first * 2; // zero-length loop piece: drop its points
    start = i;
  }
}

const nN = nodes.length / 2, nP = points.length / 2, nE = edges.length;
const buf = Buffer.alloc(8 + 4 + nN * 8 + 4 + nP * 8 + 4 + nE * 28);
let o = 0;
buf.write('MRRT', o); o += 4;
buf.writeUInt32LE(1, o); o += 4;
buf.writeUInt32LE(nN, o); o += 4;
for (let i = 0; i < nodes.length; i++) { buf.writeInt32LE(nodes[i], o); o += 4; }
buf.writeUInt32LE(nP, o); o += 4;
for (let i = 0; i < points.length; i++) { buf.writeInt32LE(points[i], o); o += 4; }
buf.writeUInt32LE(nE, o); o += 4;
for (const e of edges) {
  buf.writeUInt32LE(e[0], o); buf.writeUInt32LE(e[1], o + 4); buf.writeUInt32LE(e[2], o + 8); buf.writeUInt32LE(e[3], o + 12);
  buf.writeFloatLE(e[4], o + 16); buf.writeFloatLE(e[5], o + 20); buf.writeUInt32LE(e[6], o + 24);
  o += 28;
}
await writeFile(join(DIR, 'routes.bin'), buf);
console.log(`routes.bin: ${nN} nodes, ${nE} edges, ${nP} points, ${(buf.length / 1048576).toFixed(1)} MB`);
