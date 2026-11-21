#!/usr/bin/env node
// Real ground heights for the whole county.
//
//   node fetch_terrain.mjs [--zoom 12] [--spacing 16]
//
// Downloads Terrarium-encoded elevation tiles (Tilezen "Terrain Tiles", AWS Open Data:
// https://registry.opendata.aws/terrain-tiles/ ; UK coverage is SRTM / EU-DEM based) for the
// Greater Manchester bbox + 1 km margin, decodes the PNGs (built-in decoder, no packages),
// resamples them onto a regular grid in the game's projection and writes
//
//   Content/GeoData/terrain.bin
//     char[4]  'MRTR'
//     u32      version (1)
//     i32      minX, minY        grid origin, Unreal cm (+X east, +Y south)
//     u32      spacing           cm between samples
//     u32      nx, ny            sample counts
//     f32      baseHeightM       metres above sea level that maps to Unreal Z = 0 (height at the origin)
//     i16[ny][nx]  height        decimetres relative to baseHeightM, row-major, row 0 = minY (north)
//
// AMRGeoWorld loads this once and samples it (bilinear) for the ground mesh, roads, buildings
// and spawn points, so every location in the county has solid ground.

import { mkdir, readFile, writeFile, access } from 'node:fs/promises';
import { inflateSync } from 'node:zlib';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { GM_BBOX, ORIGIN, toLocal, toLatLon } from './geo.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const CACHE = join(HERE, 'data', 'terrain');
const OUT = join(HERE, '..', '..', 'Content', 'GeoData', 'terrain.bin');
const args = process.argv.slice(2);
const opt = (n, d) => { const i = args.indexOf(n); return i >= 0 ? Number(args[i + 1]) : d; };
const Z = opt('--zoom', 12), SPACING_M = opt('--spacing', 16), MARGIN_M = 1000;

// ------------------------------------------------------------------ PNG (8-bit RGB/RGBA, non-interlaced)

function decodePng(buf) {
  if (buf.readUInt32BE(0) !== 0x89504e47) throw new Error('not a PNG');
  let pos = 8, w = 0, h = 0, colorType = 0, depth = 0; const idat = [];
  while (pos < buf.length) {
    const len = buf.readUInt32BE(pos), type = buf.toString('latin1', pos + 4, pos + 8), data = buf.subarray(pos + 8, pos + 8 + len);
    if (type === 'IHDR') { w = data.readUInt32BE(0); h = data.readUInt32BE(4); depth = data[8]; colorType = data[9]; if (data[12]) throw new Error('interlaced PNG'); }
    else if (type === 'IDAT') idat.push(data);
    else if (type === 'IEND') break;
    pos += 12 + len;
  }
  if (depth !== 8 || (colorType !== 2 && colorType !== 6)) throw new Error(`unsupported PNG depth ${depth} type ${colorType}`);
  const bpp = colorType === 6 ? 4 : 3, stride = w * bpp;
  const raw = inflateSync(Buffer.concat(idat)), out = Buffer.alloc(h * stride);
  for (let y = 0; y < h; y++) {
    const f = raw[y * (stride + 1)], src = y * (stride + 1) + 1, dst = y * stride;
    for (let x = 0; x < stride; x++) {
      const a = x >= bpp ? out[dst + x - bpp] : 0, b = y ? out[dst - stride + x] : 0, c = x >= bpp && y ? out[dst - stride + x - bpp] : 0;
      let v = raw[src + x];
      if (f === 1) v += a; else if (f === 2) v += b; else if (f === 3) v += (a + b) >> 1;
      else if (f === 4) { const p = a + b - c, pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c); v += pa <= pb && pa <= pc ? a : pb <= pc ? b : c; }
      out[dst + x] = v & 255;
    }
  }
  const elev = new Float32Array(w * h);
  for (let i = 0; i < w * h; i++) elev[i] = out[i * bpp] * 256 + out[i * bpp + 1] + out[i * bpp + 2] / 256 - 32768;
  return { w, h, elev };
}

// ------------------------------------------------------------------ tiles

const n = 2 ** Z;
const lonToPx = (lon) => ((lon + 180) / 360) * n * 256;
const latToPx = (lat) => { const r = (lat * Math.PI) / 180; return ((1 - Math.log(Math.tan(r) + 1 / Math.cos(r)) / Math.PI) / 2) * n * 256; };
const exists = (p) => access(p).then(() => true, () => false);
const tiles = new Map();

async function loadTile(tx, ty) {
  const k = `${tx}_${ty}`;
  if (tiles.has(k)) return tiles.get(k);
  const file = join(CACHE, `${Z}_${tx}_${ty}.png`);
  if (!(await exists(file))) {
    const url = `https://s3.amazonaws.com/elevation-tiles-prod/terrarium/${Z}/${tx}/${ty}.png`;
    for (let attempt = 1; ; attempt++) {
      const res = await fetch(url);
      if (res.ok) { await writeFile(file, Buffer.from(await res.arrayBuffer())); break; }
      if (attempt >= 4) throw new Error(`${url}: HTTP ${res.status}`);
      await new Promise((r) => setTimeout(r, 1000 * attempt));
    }
  }
  const t = decodePng(await readFile(file));
  tiles.set(k, t);
  return t;
}

async function main() {
  await mkdir(CACHE, { recursive: true });
  // Projected extent of the bbox corners, plus margin.
  const corners = [[GM_BBOX.south, GM_BBOX.west], [GM_BBOX.south, GM_BBOX.east], [GM_BBOX.north, GM_BBOX.west], [GM_BBOX.north, GM_BBOX.east]].map(([la, lo]) => toLocal(la, lo));
  const minE = Math.min(...corners.map((c) => c.east)) - MARGIN_M, maxE = Math.max(...corners.map((c) => c.east)) + MARGIN_M;
  const minN = Math.min(...corners.map((c) => c.north)) - MARGIN_M, maxN = Math.max(...corners.map((c) => c.north)) + MARGIN_M;
  const nx = Math.ceil((maxE - minE) / SPACING_M) + 1, ny = Math.ceil((maxN - minN) / SPACING_M) + 1;
  // Row 0 is the northern edge (smallest Unreal Y).
  const minXcm = Math.round(minE * 100), minYcm = Math.round(-maxN * 100);

  // Pre-download every tile the grid touches.
  const sw = toLatLon(minE, minN), ne = toLatLon(maxE, maxN), nw = toLatLon(minE, maxN), se = toLatLon(maxE, minN);
  const lats = [sw.lat, ne.lat, nw.lat, se.lat], lons = [sw.lon, ne.lon, nw.lon, se.lon];
  const tx0 = Math.floor(lonToPx(Math.min(...lons)) / 256), tx1 = Math.floor(lonToPx(Math.max(...lons)) / 256);
  const ty0 = Math.floor(latToPx(Math.max(...lats)) / 256), ty1 = Math.floor(latToPx(Math.min(...lats)) / 256);
  const total = (tx1 - tx0 + 1) * (ty1 - ty0 + 1);
  console.log(`Grid ${nx} x ${ny} @ ${SPACING_M} m; ${total} terrain tiles at z${Z}`);
  let done = 0;
  for (let ty = ty0; ty <= ty1; ty++) for (let tx = tx0; tx <= tx1; tx++) { await loadTile(tx, ty); if (++done % 20 === 0) console.log(`  ${done}/${total}`); }

  const sample = (lat, lon) => {
    const px = lonToPx(lon) - 0.5, py = latToPx(lat) - 0.5;
    const x0 = Math.floor(px), y0 = Math.floor(py), fx = px - x0, fy = py - y0;
    const at = (gx, gy) => { const t = tiles.get(`${Math.floor(gx / 256)}_${Math.floor(gy / 256)}`); return t.elev[(gy & 255) * 256 + (gx & 255)]; };
    return (at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) + (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy;
  };

  const base = sample(ORIGIN.lat, ORIGIN.lon);
  const heights = new Int16Array(nx * ny);
  let lo = Infinity, hi = -Infinity, loAt = null, hiAt = null;
  for (let j = 0; j < ny; j++) {
    const north = maxN - j * SPACING_M;
    for (let i = 0; i < nx; i++) {
      const east = minE + i * SPACING_M;
      const { lat, lon } = toLatLon(east, north);
      const h = sample(lat, lon);
      if (h < lo) { lo = h; loAt = { lat, lon }; }
      if (h > hi) { hi = h; hiAt = { lat, lon }; }
      heights[j * nx + i] = Math.max(-32767, Math.min(32767, Math.round((h - base) * 10)));
    }
  }

  const header = Buffer.alloc(32);
  header.write('MRTR', 0, 'latin1'); header.writeUInt32LE(1, 4);
  header.writeInt32LE(minXcm, 8); header.writeInt32LE(minYcm, 12);
  header.writeUInt32LE(SPACING_M * 100, 16); header.writeUInt32LE(nx, 20); header.writeUInt32LE(ny, 24);
  header.writeFloatLE(base, 28);
  await mkdir(dirname(OUT), { recursive: true });
  await writeFile(OUT, Buffer.concat([header, Buffer.from(heights.buffer)]));
  console.log(JSON.stringify({
    out: OUT, nx, ny, spacingM: SPACING_M, bytes: 32 + heights.byteLength, baseHeightM: +base.toFixed(1),
    lowestM: +lo.toFixed(1), lowestAt: loAt, highestM: +hi.toFixed(1), highestAt: hiAt,
  }, null, 2));
}

main().catch((e) => { console.error(e.message); process.exit(1); });
