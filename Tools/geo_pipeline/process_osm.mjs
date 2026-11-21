#!/usr/bin/env node
// Step 2 of the geo pipeline: turn raw Overpass tiles into game-ready cells.
//
//   node process_osm.mjs [--cell 256]
//
// Output (data/cells/):
//   manifest.json          bounds, cell size, counts, boundary (projected), validation report
//   cell_<x>_<y>.json      roads, buildings, areas, props for one World Partition cell
//
// All coordinates in the output are Unreal centimetres (+X east, +Y south) relative
// to the world origin (Manchester Town Hall), produced by the same projection the
// game uses for GPS (Source/ManchesterRide/Sim/GeoProjection.cpp).

import { readdir, readFile, writeFile, mkdir, rm } from 'node:fs/promises';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { toLocal, toUnreal, pointInRings } from './geo.mjs';
import { loadTerrain } from './terrain.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const RAW = join(HERE, 'data', 'raw');
const args = process.argv.slice(2);
const opt = (name, def) => { const i = args.indexOf(name); return i >= 0 ? args[i + 1] : def; };
// --install writes straight into the game's Content/GeoData (streamed by AMRGeoWorld).
const OUT = args.includes('--install') ? join(HERE, '..', '..', 'Content', 'GeoData') : opt('--out', join(HERE, 'data', 'cells'));
const CELL_M = Number(opt('--cell', 256));
// Ground heights (fetch_terrain.mjs). Used for bridge decks; the game samples it for everything else.
const TERRAIN = loadTerrain(join(HERE, '..', '..', 'Content', 'GeoData', 'terrain.bin'));

// ---------------------------------------------------------------- classification

// Surfaces match MRSim::ESurface / Config/DefaultEngine.ini SurfaceType1..8.
const SURFACE = { asphalt: 0, concrete: 1, gravel: 2, grass: 3, mud: 4, dirt: 5, forest: 6, cobbles: 7 };

const ROAD_CLASSES = {
  motorway:       { width: 22.0, lanes: 6, kind: 'motorway' },
  motorway_link:  { width: 7.0,  lanes: 2, kind: 'motorway' },
  trunk:          { width: 14.0, lanes: 4, kind: 'major' },
  trunk_link:     { width: 7.0,  lanes: 2, kind: 'major' },
  primary:        { width: 9.0,  lanes: 2, kind: 'major' },
  primary_link:   { width: 6.5,  lanes: 1, kind: 'major' },
  secondary:      { width: 7.5,  lanes: 2, kind: 'major' },
  secondary_link: { width: 6.0,  lanes: 1, kind: 'major' },
  tertiary:       { width: 7.0,  lanes: 2, kind: 'minor' },
  tertiary_link:  { width: 5.5,  lanes: 1, kind: 'minor' },
  unclassified:   { width: 6.0,  lanes: 2, kind: 'minor' },
  residential:    { width: 5.5,  lanes: 2, kind: 'residential' },
  living_street:  { width: 5.0,  lanes: 1, kind: 'residential' },
  service:        { width: 4.0,  lanes: 1, kind: 'service' },
  pedestrian:     { width: 6.0,  lanes: 0, kind: 'pedestrian' },
  track:          { width: 3.0,  lanes: 1, kind: 'track', surface: 'gravel' },
  cycleway:       { width: 2.5,  lanes: 0, kind: 'cycleway' },
  footway:        { width: 2.0,  lanes: 0, kind: 'footway' },
  path:           { width: 1.8,  lanes: 0, kind: 'path', surface: 'dirt' },
  bridleway:      { width: 2.5,  lanes: 0, kind: 'path', surface: 'dirt' },
  steps:          { width: 2.0,  lanes: 0, kind: 'steps', surface: 'concrete' },
};

function surfaceFromTag(tag, fallback) {
  if (!tag) return SURFACE[fallback] ?? SURFACE.asphalt;
  if (/asphalt|paved|chipseal/.test(tag)) return SURFACE.asphalt;
  if (/concrete|paving_stones|metal|wood/.test(tag)) return SURFACE.concrete;
  if (/sett|cobble|unhewn/.test(tag)) return SURFACE.cobbles;
  if (/gravel|compacted|pebble|unpaved|rock/.test(tag)) return SURFACE.gravel;
  if (/grass/.test(tag)) return SURFACE.grass;
  if (/mud|sand/.test(tag)) return SURFACE.mud;
  if (/dirt|earth|ground|woodchips/.test(tag)) return SURFACE.dirt;
  return SURFACE[fallback] ?? SURFACE.asphalt;
}

// Heights: real tags first, then UK-typical defaults by building type.
const BUILDING_DEFAULTS = {
  house: { h: 7.5, style: 'house' }, terrace: { h: 8.0, style: 'terraced' }, semidetached_house: { h: 7.5, style: 'semi' },
  detached: { h: 7.5, style: 'detached' }, bungalow: { h: 4.5, style: 'bungalow' }, residential: { h: 8.0, style: 'house' },
  apartments: { h: 15.0, style: 'apartments' }, flats: { h: 15.0, style: 'apartments' }, dormitory: { h: 15.0, style: 'apartments' },
  commercial: { h: 12.0, style: 'commercial' }, office: { h: 18.0, style: 'office' }, retail: { h: 7.0, style: 'retail' },
  supermarket: { h: 8.0, style: 'retail' }, industrial: { h: 9.0, style: 'industrial' }, warehouse: { h: 10.0, style: 'industrial' },
  church: { h: 14.0, style: 'church' }, cathedral: { h: 25.0, style: 'church' }, school: { h: 9.0, style: 'civic' },
  university: { h: 16.0, style: 'civic' }, hospital: { h: 18.0, style: 'civic' }, train_station: { h: 14.0, style: 'civic' },
  garage: { h: 2.6, style: 'garage' }, garages: { h: 2.6, style: 'garage' }, shed: { h: 2.5, style: 'garage' },
  stadium: { h: 35.0, style: 'stadium' }, hotel: { h: 20.0, style: 'office' }, mill: { h: 22.0, style: 'mill' },
};

// Landmarks get hand-made models later (Milestone 5); flag them so the importer can swap meshes.
const LANDMARKS = [
  /^beetham tower$/i, /manchester town hall/i, /manchester cathedral/i, /central library/i, /^old trafford$/i, /^etihad stadium$/i,
  /deansgate square/i, /mediacity/i, /^the lowry$/i, /imperial war museum/i, /^(the )?trafford centre$/i, /stockport viaduct/i,
  /bolton town hall/i, /wigan pier/i, /ashton town hall/i, /rochdale town hall/i, /oldham town hall/i, /bury town hall/i,
  /^manchester piccadilly$/i, /^manchester victoria$/i, /^john rylands/i, /palace hotel/i, /^cis tower$/i, /^co-operative insurance tower$/i, /^one angel square$/i,
];

function buildingHeight(t) {
  if (t.height) { const h = parseFloat(t.height); if (h > 0) return h; }
  if (t['building:levels']) { const l = parseFloat(t['building:levels']); if (l > 0) return l * 3.1 + (t['roof:levels'] ? parseFloat(t['roof:levels']) * 2.5 : 1.0); }
  return (BUILDING_DEFAULTS[t.building] ?? { h: 7.5 }).h;
}

function areaClass(t) {
  const lu = t.landuse, nat = t.natural, lei = t.leisure;
  if (nat === 'water' || lu === 'reservoir' || lu === 'basin') return { kind: 'water' };
  if (nat === 'wood' || lu === 'forest') return { kind: 'woodland', surface: SURFACE.forest };
  if (nat === 'scrub' || nat === 'heath' || nat === 'wetland') return { kind: 'scrub', surface: SURFACE.mud };
  if (lei === 'park' || lei === 'garden' || lei === 'recreation_ground' || lei === 'nature_reserve' || lu === 'grass' || lu === 'recreation_ground'
    || lu === 'meadow' || lu === 'village_green' || nat === 'grassland' || lu === 'cemetery' || lei === 'golf_course') return { kind: 'green', surface: SURFACE.grass };
  if (lei === 'pitch') return { kind: 'pitch', surface: SURFACE.grass };
  if (lu === 'farmland' || lu === 'farmyard' || lu === 'allotments' || lu === 'orchard') return { kind: 'farmland', surface: SURFACE.grass };
  if (lu === 'industrial' || lu === 'commercial' || lu === 'retail' || lu === 'railway' || lu === 'construction' || t.amenity === 'parking')
    return { kind: lu === 'retail' ? 'retail' : (t.amenity === 'parking' ? 'car_park' : 'industrial'), surface: SURFACE.concrete };
  if (lu === 'residential') return { kind: 'residential' };
  if (lu === 'quarry' || nat === 'bare_rock') return { kind: 'quarry', surface: SURFACE.gravel };
  return null;
}

// ---------------------------------------------------------------- geometry

const proj = (pt) => { const l = toLocal(pt.lat, pt.lon); const u = toUnreal(l.east, l.north); return [Math.round(u.x), Math.round(u.y)]; };
const cellOf = ([x, y]) => [Math.floor(x / (CELL_M * 100)), Math.floor(y / (CELL_M * 100))];
const key = ([cx, cy]) => `${cx}_${cy}`;

function polygonAreaM2(pts) {
  let a = 0;
  for (let i = 0, j = pts.length - 1; i < pts.length; j = i++) a += (pts[j][0] + pts[i][0]) * (pts[j][1] - pts[i][1]);
  return Math.abs(a / 2) / 10000;
}
function centroid(pts) {
  let x = 0, y = 0;
  for (const p of pts) { x += p[0]; y += p[1]; }
  return [x / pts.length, y / pts.length];
}

/** Split a projected polyline into per-cell pieces (pieces share their boundary vertex). */
function splitByCell(pts) {
  const pieces = [];
  let cur = [pts[0]], curKey = key(cellOf(pts[0]));
  for (let i = 1; i < pts.length; i++) {
    const k = key(cellOf(pts[i]));
    cur.push(pts[i]);
    if (k !== curKey) { pieces.push({ cell: curKey, pts: cur }); cur = [pts[i]]; curKey = k; }
  }
  if (cur.length > 1) pieces.push({ cell: curKey, pts: cur });
  return pieces;
}

/** Sutherland-Hodgman clip of a ring against an axis-aligned rectangle (works for concave rings). */
function clipRing(ring, x0, y0, x1, y1) {
  let out = ring;
  const edges = [[(p) => p[0] >= x0, (a, b) => lerpX(a, b, x0)], [(p) => p[0] <= x1, (a, b) => lerpX(a, b, x1)],
    [(p) => p[1] >= y0, (a, b) => lerpY(a, b, y0)], [(p) => p[1] <= y1, (a, b) => lerpY(a, b, y1)]];
  for (const [inside, cut] of edges) {
    const input = out; out = [];
    for (let i = 0; i < input.length; i++) {
      const cur = input[i], prev = input[(i + input.length - 1) % input.length];
      if (inside(cur)) { if (!inside(prev)) out.push(cut(prev, cur)); out.push(cur); }
      else if (inside(prev)) out.push(cut(prev, cur));
    }
    if (out.length < 3) return [];
  }
  return out;
}
const lerpX = (a, b, x) => { const t = (x - a[0]) / (b[0] - a[0]); return [Math.round(x), Math.round(a[1] + (b[1] - a[1]) * t)]; };
const lerpY = (a, b, y) => { const t = (y - a[1]) / (b[1] - a[1]); return [Math.round(a[0] + (b[0] - a[0]) * t), Math.round(y)]; };
// Area kinds the game paints onto the ground (others, e.g. landuse=residential, keep the default ground).
const PAINTED_AREAS = new Set(['water', 'woodland', 'scrub', 'green', 'pitch', 'farmland', 'retail', 'car_park', 'industrial', 'quarry']);

// ---------------------------------------------------------------- boundary

async function loadBoundary() {
  try {
    const data = JSON.parse(await readFile(join(RAW, 'boundary.json'), 'utf8'));
    const rel = data.elements?.find((e) => e.type === 'relation');
    if (!rel) return null;
    // Stitch outer member ways into closed rings.
    const segs = rel.members.filter((m) => m.type === 'way' && m.role === 'outer' && m.geometry).map((m) => m.geometry.map((g) => [g.lon, g.lat]));
    const rings = [];
    while (segs.length) {
      let ring = segs.shift();
      let grew = true;
      while (grew && !(ring.length > 3 && ring[0][0] === ring.at(-1)[0] && ring[0][1] === ring.at(-1)[1])) {
        grew = false;
        for (let i = 0; i < segs.length; i++) {
          const s = segs[i];
          const end = ring.at(-1);
          if (s[0][0] === end[0] && s[0][1] === end[1]) { ring = ring.concat(s.slice(1)); }
          else if (s.at(-1)[0] === end[0] && s.at(-1)[1] === end[1]) { ring = ring.concat(s.slice(0, -1).reverse()); }
          else continue;
          segs.splice(i, 1); grew = true; break;
        }
      }
      rings.push(ring);
    }
    return rings;
  } catch { return null; }
}

// ---------------------------------------------------------------- main

async function main() {
  const files = (await readdir(RAW).catch(() => [])).filter((f) => f.startsWith('tile_') && f.endsWith('.json'));
  if (!files.length) throw new Error(`No tiles in ${RAW}. Run fetch_osm.mjs first.`);
  const boundary = await loadBoundary();

  const seen = new Set();
  const cells = new Map();
  const cell = (k) => { if (!cells.has(k)) cells.set(k, { roads: [], buildings: [], areas: [], props: [], places: [] }); return cells.get(k); };
  const stats = { roads: 0, roadKm: 0, buildings: 0, areas: 0, props: 0, places: 0, skippedOutside: 0, motorways: new Set(), landmarks: [] };
  let minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;

  for (const f of files) {
    const data = JSON.parse(await readFile(join(RAW, f), 'utf8'));
    for (const el of data.elements) {
      const id = `${el.type}/${el.id}`;
      if (seen.has(id)) continue; // features overlapping tile edges appear twice
      seen.add(id);
      const t = el.tags || {};

      if (el.type === 'node') {
        if (boundary && !pointInRings(el.lon, el.lat, boundary)) { stats.skippedOutside++; continue; }
        const p = proj(el);
        if (t.place) { cell(key(cellOf(p))).places.push({ name: t.name, type: t.place, p }); stats.places++; }
        else { cell(key(cellOf(p))).props.push({ type: t.highway || t.amenity, p, dir: t.direction }); stats.props++; }
        continue;
      }
      if (el.type !== 'way' || !el.geometry || el.geometry.length < 2) continue;
      const mid = el.geometry[Math.floor(el.geometry.length / 2)];
      if (boundary && !pointInRings(mid.lon, mid.lat, boundary)) { stats.skippedOutside++; continue; }
      const pts = el.geometry.map(proj);
      for (const [x, y] of pts) { minX = Math.min(minX, x); minY = Math.min(minY, y); maxX = Math.max(maxX, x); maxY = Math.max(maxY, y); }

      if (t.highway && ROAD_CLASSES[t.highway] && t.area !== 'yes') {
        const rc = ROAD_CLASSES[t.highway];
        const lanes = parseInt(t.lanes, 10) || rc.lanes;
        const width = parseFloat(t.width) || (t.lanes ? Math.max(rc.width, lanes * 3.3) : rc.width);
        const road = {
          id: el.id, cls: t.highway, kind: rc.kind, name: t.name, ref: t.ref, lanes, width,
          oneway: t.oneway === 'yes' || t.highway === 'motorway' || t.junction === 'roundabout',
          roundabout: t.junction === 'roundabout', bridge: t.bridge && t.bridge !== 'no', tunnel: t.tunnel && t.tunnel !== 'no',
          layer: parseInt(t.layer, 10) || 0, surface: surfaceFromTag(t.surface, rc.surface), maxspeed: t.maxspeed,
          cycleLane: !!(t['cycleway'] || t['cycleway:left'] || t['cycleway:both']), lit: t.lit === 'yes',
        };
        if (/^M\d+/.test(t.ref || '')) stats.motorways.add(t.ref.split(';')[0].trim());
        let len = 0;
        for (let i = 1; i < pts.length; i++) len += Math.hypot(pts[i][0] - pts[i - 1][0], pts[i][1] - pts[i - 1][1]);
        stats.roadKm += len / 100000;
        if (road.bridge && TERRAIN && pts.length >= 2) {
          const z0 = TERRAIN.heightCm(...pts[0]), z1 = TERRAIN.heightCm(...pts.at(-1)), lift = Math.max(0, road.layer) * 50;
          let acc = 0; const d = [0];
          for (let i = 1; i < pts.length; i++) d.push(acc += Math.hypot(pts[i][0] - pts[i - 1][0], pts[i][1] - pts[i - 1][1]));
          pts.forEach((p, i) => { p.push(Math.round(z0 + (z1 - z0) * (acc ? d[i] / acc : 0) + lift)); });
          stats.bridges = (stats.bridges || 0) + 1;
        }
        for (const piece of splitByCell(pts)) cell(piece.cell).roads.push({ ...road, pts: piece.pts });
        stats.roads++;
      } else if (t.building) {
        if (pts.length < 4) continue;
        const ring = pts.slice(0, -1); // OSM closed ways repeat the first node
        const c = centroid(ring);
        const def = BUILDING_DEFAULTS[t.building] ?? { style: 'generic' };
        const isLandmark = !!(t.name && LANDMARKS.some((r) => r.test(t.name)));
        const b = {
          id: el.id, type: t.building, style: def.style, h: buildingHeight(t), minH: parseFloat(t.min_height) || 0,
          roof: t['roof:shape'] || (def.style === 'terraced' || def.style === 'semi' || def.style === 'house' || def.style === 'detached' ? 'gabled' : 'flat'),
          material: t['building:material'] || (['terraced', 'semi', 'house', 'mill', 'church'].includes(def.style) ? 'brick' : undefined),
          colour: t['building:colour'], name: t.name, landmark: isLandmark || undefined,
          areaM2: Math.round(polygonAreaM2(ring)), ring,
        };
        if (isLandmark) stats.landmarks.push(t.name);
        cell(key(cellOf(c))).buildings.push(b);
        stats.buildings++;
      } else if (t.waterway || t.railway) {
        const line = { id: el.id, kind: t.waterway ? `waterway:${t.waterway}` : `railway:${t.railway}`, name: t.name, bridge: !!t.bridge, tunnel: !!t.tunnel };
        for (const piece of splitByCell(pts)) cell(piece.cell).roads.push({ ...line, pts: piece.pts });
      } else {
        const ac = areaClass(t);
        if (!ac || pts.length < 4) continue;
        if (!PAINTED_AREAS.has(ac.kind)) continue;
        // Store a clipped copy in every cell the area overlaps, so a park or wood that spans many
        // cells is painted (and given its surface grip) in all of them.
        const ring = pts.slice(0, -1), C = CELL_M * 100;
        const xs = ring.map((p) => p[0]), ys = ring.map((p) => p[1]);
        const [cx0, cy0] = cellOf([Math.min(...xs), Math.min(...ys)]), [cx1, cy1] = cellOf([Math.max(...xs), Math.max(...ys)]);
        for (let cy = cy0; cy <= cy1; cy++) for (let cx = cx0; cx <= cx1; cx++) {
          const part = (cx0 === cx1 && cy0 === cy1) ? ring : clipRing(ring, cx * C, cy * C, (cx + 1) * C, (cy + 1) * C);
          if (part.length >= 3) cell(`${cx}_${cy}`).areas.push({ id: el.id, ...ac, name: t.name, ring: part });
        }
        stats.areas++;
      }
    }
  }

  // Replace old cells but keep terrain.bin (written by fetch_terrain.mjs) in the same folder.
  await mkdir(OUT, { recursive: true });
  for (const f of await readdir(OUT)) if (/^cell_.*.json$|^manifest.json$/.test(f)) await rm(join(OUT, f));
  for (const [k, c] of cells) await writeFile(join(OUT, `cell_${k}.json`), JSON.stringify(c));

  // Validation: the projected boundary area should match the official ~1,276 km2.
  let boundaryReport = null;
  if (boundary) {
    const projRings = boundary.map((r) => r.map(([lon, lat]) => proj({ lat, lon })));
    const outer = projRings.reduce((a, r) => a + polygonAreaM2(r), 0) / 1e6;
    const xs = projRings.flat().map((p) => p[0]), ys = projRings.flat().map((p) => p[1]);
    boundaryReport = {
      areaKm2: Number(outer.toFixed(1)), expectedKm2: 1276, errorPercent: Number(((outer - 1276) / 1276 * 100).toFixed(2)),
      extentKm: { eastWest: Number(((Math.max(...xs) - Math.min(...xs)) / 1e5).toFixed(2)), northSouth: Number(((Math.max(...ys) - Math.min(...ys)) / 1e5).toFixed(2)) },
      rings: projRings,
    };
  }
  const manifest = {
    generated: new Date().toISOString(), origin: { lat: 53.47936, lon: -2.24484, name: 'Manchester Town Hall' },
    units: 'Unreal cm, +X east, +Y south', cellSizeM: CELL_M, cellCount: cells.size,
    boundsCm: { minX, minY, maxX, maxY },
    stats: { ...stats, roadKm: Number(stats.roadKm.toFixed(2)), motorways: [...stats.motorways].sort(), landmarks: [...new Set(stats.landmarks)] },
    boundary: boundaryReport,
    attribution: 'Map data (c) OpenStreetMap contributors, ODbL 1.0',
  };
  await writeFile(join(OUT, 'manifest.json'), JSON.stringify(manifest, null, 1));
  const { rings, ...printable } = boundaryReport ?? {};
  console.log(JSON.stringify({ ...manifest, boundary: boundaryReport ? printable : null }, null, 2));
}

main().catch((e) => { console.error(e.message); process.exit(1); });
