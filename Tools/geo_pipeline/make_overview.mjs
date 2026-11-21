#!/usr/bin/env node
// Builds Content/GeoData/overview.json: a light, county-wide map for the in-game full map (M).
//
//   node make_overview.mjs
//
// Reads every cell_<x>_<y>.json the game streams and keeps only what a road map needs:
//   lines   [kind, x0, y0, x1, y1, ...]  kind 0 motorway, 1 A/B road, 2 local street, 3 rail,
//                                        4 tram, 5 river, 6 canal. Metres, Unreal axes (+X east, +Y south).
//   places  [name, type, x, y]           city/town/suburb/village/neighbourhood names
//   labels  [name, x, y, angleDeg, kind] one street-name label per road per ~600 m
// Lines are simplified (Douglas-Peucker) so the whole county fits in a few MB.

import { readdir, readFile, writeFile } from 'node:fs/promises';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const DIR = join(HERE, '..', '..', 'Content', 'GeoData');

function kindOf(r) {
  const k = r.kind || '';
  if (k === 'motorway') return 0;
  if (k === 'major') return 1;
  if (k === 'minor' || k === 'residential') return 2;
  if (k === 'railway:rail') return 3;
  if (k === 'railway:light_rail' || k === 'railway:tram') return 4;
  if (k === 'waterway:river') return 5;
  if (k === 'waterway:canal') return 6;
  return -1; // footways, service roads, paths, streams: too dense for the overview
}
const TOL = [6, 6, 5, 8, 6, 10, 8]; // metres

function simplify(pts, tol) {
  if (pts.length <= 2) return pts;
  const keep = new Uint8Array(pts.length); keep[0] = keep[pts.length - 1] = 1;
  const stack = [[0, pts.length - 1]];
  while (stack.length) {
    const [a, b] = stack.pop();
    const [ax, ay] = pts[a], [bx, by] = pts[b];
    const dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy || 1e-9;
    let best = -1, bi = -1;
    for (let i = a + 1; i < b; i++) {
      const t = Math.max(0, Math.min(1, ((pts[i][0] - ax) * dx + (pts[i][1] - ay) * dy) / len2));
      const ex = ax + t * dx - pts[i][0], ey = ay + t * dy - pts[i][1];
      const d = ex * ex + ey * ey;
      if (d > best) { best = d; bi = i; }
    }
    if (best > tol * tol) { keep[bi] = 1; stack.push([a, bi], [bi, b]); }
  }
  return pts.filter((_, i) => keep[i]);
}

const files = (await readdir(DIR)).filter((f) => /^cell_-?\d+_-?\d+\.json$/.test(f));
const lines = [];
const places = new Map();
const labelBest = new Map(); // name|tileX|tileY -> {len, x, y, ang, kind}
let n = 0;
for (const f of files) {
  const c = JSON.parse(await readFile(join(DIR, f), 'utf8'));
  for (const r of c.roads || []) {
    const k = kindOf(r);
    if (k < 0 || r.tunnel) continue;
    const pts = (r.pts || []).map((p) => [p[0] / 100, p[1] / 100]);
    if (pts.length < 2) continue;
    const s = simplify(pts, TOL[k]);
    const flat = [k];
    for (const [x, y] of s) flat.push(Math.round(x), Math.round(y));
    lines.push(flat);
    if (r.name && k <= 2) {
      // Longest straight piece of this road in each 600 m tile carries its name label.
      for (let i = 0; i + 1 < pts.length; i++) {
        const [ax, ay] = pts[i], [bx, by] = pts[i + 1];
        const len = Math.hypot(bx - ax, by - ay);
        if (len < 25) continue;
        const mx = (ax + bx) / 2, my = (ay + by) / 2;
        const key = `${r.name}|${Math.floor(mx / 600)}|${Math.floor(my / 600)}`;
        const prev = labelBest.get(key);
        if (!prev || len > prev.len) {
          let ang = Math.atan2(by - ay, bx - ax) * 180 / Math.PI;
          if (ang > 90) ang -= 180; if (ang < -90) ang += 180; // keep text upright
          labelBest.set(key, { len, x: Math.round(mx), y: Math.round(my), ang: Math.round(ang), kind: k, name: r.name });
        }
      }
    }
  }
  for (const p of c.places || []) {
    if (!p.name || !p.p) continue;
    if (!places.has(p.name)) places.set(p.name, [p.name, p.type || 'place', Math.round(p.p[0] / 100), Math.round(p.p[1] / 100)]);
  }
  if (++n % 2000 === 0) process.stdout.write(`  ${n}/${files.length} cells\n`);
}
const labels = [...labelBest.values()].map((l) => [l.name, l.x, l.y, l.ang, l.kind]);
const out = { v: 1, units: 'm, Unreal axes (+X east, +Y south)', lines, places: [...places.values()], labels };
await writeFile(join(DIR, 'overview.json'), JSON.stringify(out));
const pts = lines.reduce((a, l) => a + (l.length - 1) / 2, 0);
console.log(`overview.json: ${lines.length} lines (${pts} points), ${out.places.length} places, ${labels.length} street labels`);
