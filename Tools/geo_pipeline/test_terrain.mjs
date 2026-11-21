#!/usr/bin/env node
// Checks Content/GeoData/terrain.bin against published spot heights.
// Source DEM is ~30 m resolution, so summits read a little low and tolerances are generous.
import { readFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { toLocal, toUnreal } from './geo.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const buf = readFileSync(join(HERE, '..', '..', 'Content', 'GeoData', 'terrain.bin'));
if (buf.toString('latin1', 0, 4) !== 'MRTR') throw new Error('bad magic');
const minX = buf.readInt32LE(8), minY = buf.readInt32LE(12), sp = buf.readUInt32LE(16), nx = buf.readUInt32LE(20), ny = buf.readUInt32LE(24), base = buf.readFloatLE(28);
const h = new Int16Array(buf.buffer.slice(buf.byteOffset + 32, buf.byteOffset + 32 + nx * ny * 2));

// Same bilinear sampling as AMRGeoWorld::TerrainHeightAt (returns metres above sea level).
function heightAt(lat, lon) {
  const l = toLocal(lat, lon), u = toUnreal(l.east, l.north);
  const gx = (u.x - minX) / sp, gy = (u.y - minY) / sp;
  const x0 = Math.floor(gx), y0 = Math.floor(gy), fx = gx - x0, fy = gy - y0;
  const at = (x, y) => h[Math.min(ny - 1, Math.max(0, y)) * nx + Math.min(nx - 1, Math.max(0, x))] / 10;
  return base + (at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) + (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy;
}

const SPOTS = [
  // name, lat, lon, published height (m), tolerance (m), [summit search radius m: test the highest point within it,
  // because our summit coordinates are approximate]
  ['Albert Square (origin)', 53.47936, -2.24484, 39, 6],
  ['Black Chew Head (GM high point) summit', 53.5016, -1.9227, 542, 30, 1200],
  ['Winter Hill summit', 53.6261, -2.5156, 456, 30, 1000],
  ['Werneth Low', 53.4337, -2.0531, 280, 30],
  ['Knowl Hill summit', 53.6440, -2.2550, 418, 30, 1500],
  ['Dove Stone Reservoir', 53.5303, -1.9733, 250, 30],
  ['Piccadilly Gardens', 53.4810, -2.2367, 45, 8],
  ['Carrington Moss (flat lowland)', 53.4246, -2.3780, 18, 10],
];

let fails = 0;
for (const [name, lat, lon, ref, tol, radius] of SPOTS) {
  let v = heightAt(lat, lon);
  if (radius) for (let dy = -radius; dy <= radius; dy += 16) for (let dx = -radius; dx <= radius; dx += 16)
    v = Math.max(v, heightAt(lat + dy / 111320, lon + dx / (111320 * Math.cos(lat * Math.PI / 180))));
  const ok = Math.abs(v - ref) <= tol;
  if (!ok) fails++;
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${name.padEnd(34)} ${v.toFixed(1).padStart(6)} m  (published ${ref} m ±${tol})`);
}
console.log(`grid ${nx}x${ny} @ ${sp / 100} m, base ${base.toFixed(1)} m`);
process.exit(fails ? 1 : 0);
