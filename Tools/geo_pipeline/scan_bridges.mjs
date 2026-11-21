#!/usr/bin/env node
// Checks every bridge in the map for a step where it joins an ordinary road.
//   node scan_bridges.mjs
// "before" is the deck height as stored (the pipeline lifts decks 50 cm per OSM layer);
// "after" applies the game's rule (Ribbon in MRGeoWorld.cpp): a deck within 2.5 m of the
// ground lies on the ground and takes its own height over the next 2 m of clearance.
import { readdir, readFile } from 'node:fs/promises';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { loadTerrain } from './terrain.mjs';
const HERE = dirname(fileURLToPath(import.meta.url));
const DIR = join(HERE, '..', '..', 'Content', 'GeoData');
const T = loadTerrain(join(DIR, 'terrain.bin'));
const files = (await readdir(DIR)).filter((f) => /^cell_-?\d+_-?\d+\.json$/.test(f));
let bridges = 0, joins = 0, before = 0, after = 0, worstBefore = 0, worstAfter = 0; const worst = [];
for (const f of files) {
  const c = JSON.parse(await readFile(join(DIR, f), 'utf8'));
  const roads = (c.roads || []).filter((r) => r.pts && r.pts.length >= 2 && !r.tunnel);
  const ground = new Set();
  for (const r of roads) if (r.pts[0].length < 3) for (const p of r.pts) ground.add(p[0] + ',' + p[1]);
  for (const r of roads) {
    if (r.pts[0].length < 3) continue;
    bridges++;
    for (const p of [r.pts[0], r.pts.at(-1)]) {
      if (!ground.has(p[0] + ',' + p[1])) continue; // cell split or another bridge piece
      joins++;
      const d = p[2] - T.heightCm(p[0], p[1]);
      const raised = Math.min(1, Math.max(0, (d - 250) / 200)) * d;
      if (Math.abs(d) > 30) before++;
      if (Math.abs(raised) > 30) { after++; if (worst.length < 10) worst.push(`${f} ${r.name || r.kind} step ${Math.round(raised)} cm at ${p[0]},${p[1]}`); }
      worstBefore = Math.max(worstBefore, Math.abs(d)); worstAfter = Math.max(worstAfter, Math.abs(raised));
    }
  }
}
console.log(`${bridges} bridge pieces, ${joins} joins with ordinary roads`);
console.log(`steps over 30 cm before: ${before} (worst ${Math.round(worstBefore)} cm)`);
console.log(`steps over 30 cm after:  ${after} (worst ${Math.round(worstAfter)} cm)`);
console.log(worst.join('\n'));
