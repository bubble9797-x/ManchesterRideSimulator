#!/usr/bin/env node
// Step 1 of the geo pipeline: download OpenStreetMap data for Greater Manchester
// from the public Overpass API, in tiles, with caching and polite rate limiting.
//
//   node fetch_osm.mjs --boundary                 # GM county boundary polygon
//   node fetch_osm.mjs --bbox 53.4795,-2.2385,53.4830,-2.2320   # a small test area
//   node fetch_osm.mjs --all [--tile 3000]        # the whole county (~1,300 km2; hours)
//
// For the full county a Geofabrik extract (england/greater-manchester-latest.osm.pbf)
// converted with osmium to GeoJSON is faster; see README.md. Overpass is used here
// because it needs no extra tools.
//
// Licence: OSM data is (c) OpenStreetMap contributors, ODbL 1.0. Attribution is
// shown in the game's credits and DEPENDENCIES.md.

import { mkdir, writeFile, access } from 'node:fs/promises';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { GM_BBOX, tileBBox } from './geo.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const RAW = join(HERE, 'data', 'raw');
const ENDPOINT = process.env.OVERPASS_URL || 'https://overpass-api.de/api/interpreter';
const USER_AGENT = 'ManchesterRidingSimulator-geo-pipeline/0.1';

const args = process.argv.slice(2);
const flag = (name) => args.includes(name);
const opt = (name, def) => { const i = args.indexOf(name); return i >= 0 ? args[i + 1] : def; };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const exists = (p) => access(p).then(() => true, () => false);

function featureQuery(b) {
  const bb = `${b.south},${b.west},${b.north},${b.east}`;
  return `[out:json][timeout:180];
(
  way["highway"](${bb});
  way["building"](${bb});
  way["landuse"](${bb});
  way["natural"~"^(wood|scrub|grassland|heath|water|wetland|bare_rock)$"](${bb});
  way["leisure"~"^(park|pitch|golf_course|recreation_ground|nature_reserve|garden|track)$"](${bb});
  way["waterway"~"^(river|canal|stream)$"](${bb});
  way["railway"~"^(rail|light_rail|tram)$"](${bb});
  way["amenity"="parking"](${bb});
  node["highway"~"^(traffic_signals|bus_stop|crossing|street_lamp|give_way|stop)$"](${bb});
  node["amenity"~"^(post_box|bench|bicycle_parking|telephone)$"](${bb});
  node["place"~"^(city|town|suburb|village|neighbourhood|quarter)$"](${bb});
);
out body geom qt;`;
}

// In OSM, Greater Manchester is relation 88084: boundary=administrative, admin_level=5
// (designation=combined_authority; same footprint as the ten metropolitan boroughs).
const BOUNDARY_QUERY = `[out:json][timeout:300];
relation["boundary"="administrative"]["admin_level"~"^(5|6)$"]["name"="Greater Manchester"];
out body geom;`;

async function overpass(query, attempt = 1) {
  const res = await fetch(ENDPOINT, {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded', 'User-Agent': USER_AGENT },
    body: 'data=' + encodeURIComponent(query),
  });
  if ([429, 502, 503, 504].includes(res.status)) { // server busy: exponential-ish backoff, as Overpass asks
    if (attempt > 6) throw new Error(`Overpass busy after ${attempt} attempts (HTTP ${res.status})`);
    const wait = 15000 * attempt;
    console.warn(`  Overpass HTTP ${res.status}, retrying in ${wait / 1000}s`);
    await sleep(wait);
    return overpass(query, attempt + 1);
  }
  if (!res.ok) throw new Error(`Overpass HTTP ${res.status}: ${(await res.text()).slice(0, 300)}`);
  return res.json();
}

async function main() {
  await mkdir(RAW, { recursive: true });

  if (flag('--boundary')) {
    const out = join(RAW, 'boundary.json');
    console.log('Fetching Greater Manchester boundary relation...');
    const data = await overpass(BOUNDARY_QUERY);
    await writeFile(out, JSON.stringify(data));
    console.log(`  wrote ${out} (${data.elements?.length ?? 0} element(s))`);
  }

  let tiles = [];
  if (opt('--bbox')) {
    const [south, west, north, east] = opt('--bbox').split(',').map(Number);
    if ([south, west, north, east].some(Number.isNaN)) throw new Error('--bbox expects south,west,north,east');
    tiles = [{ ix: 'bbox', iy: `${south}_${west}_${north}_${east}`.replace(/[.-]/g, (c) => (c === '.' ? 'p' : 'm')), south, west, north, east }];
  } else if (flag('--all')) {
    tiles = tileBBox(GM_BBOX, Number(opt('--tile', 3000)));
  }

  let n = 0;
  for (const t of tiles) {
    n++;
    const out = join(RAW, `tile_${t.ix}_${t.iy}.json`);
    if (await exists(out)) { console.log(`[${n}/${tiles.length}] cached ${out}`); continue; }
    console.log(`[${n}/${tiles.length}] tile ${t.ix},${t.iy}  ${t.south.toFixed(4)},${t.west.toFixed(4)} -> ${t.north.toFixed(4)},${t.east.toFixed(4)}`);
    const data = await overpass(featureQuery(t));
    data.bbox = t;
    await writeFile(out, JSON.stringify(data));
    console.log(`  ${data.elements.length} elements`);
    if (n < tiles.length) await sleep(Number(opt('--delay', 3000))); // be polite to the shared public server
  }
  if (!tiles.length && !flag('--boundary')) console.log('Nothing to do. Use --boundary, --bbox s,w,n,e or --all.');
}

main().catch((e) => { console.error(e.message); process.exit(1); });
