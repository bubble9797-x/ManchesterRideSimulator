# Geo pipeline: OpenStreetMap → game cells

Plain Node.js (≥ 18). No npm packages.

| Step | Command | Output |
|---|---|---|
| 1. Boundary | `node fetch_osm.mjs --boundary` | `data/raw/boundary.json` (OSM relation 88084) |
| 2. Download | `node fetch_osm.mjs --bbox S,W,N,E` or `--all [--tile 3000]` | `data/raw/tile_*.json` (cached; re-runs skip existing tiles) |
| 3. Process | `node process_osm.mjs --install [--cell 256]` | `../../Content/GeoData/cell_X_Y.json` + `manifest.json` |
| 2b. Full county (fast) | `node pbf_to_tiles.mjs` after downloading the Geofabrik extract (below) | `data/raw/tile_pbf_*.json` (same format as step 2) |
| 2c. Terrain | `node fetch_terrain.mjs` | `../../Content/GeoData/terrain.bin` (16 m grid, int16 dm) |
| Test | `node test_geo.mjs` | projection checks |
| Test | `node test_terrain.mjs` | heights vs published spot heights |
| Test | `node --max-old-space-size=4096 test_ground.mjs --cells 3000` | no-phasing checks (port of MRGeoWorld ground/road/building rules) |

**Projection:** Transverse Mercator (WGS84) centred on Manchester Town Hall,
identical to `Source/ManchesterRide/Sim/GeoProjection.cpp`. Output is in Unreal cm,
+X east, +Y south.

**What processing does:** de-duplicates features that cross tile edges; drops
features outside the county boundary; classifies roads (motorway → steps) with
widths, lanes, oneway, roundabouts, bridges, tunnels and layers; maps OSM
`surface` to the game's 8 physics surfaces; gives buildings heights (from
`height` or `building:levels`, otherwise UK-typical defaults by type) plus style,
roof and material; classifies land use (parks, woodland, water, industrial, car
parks…); flags landmarks; splits everything into 256 m cells.

**Validation report** (in `manifest.json`, printed after processing): boundary
area against the official 1,276 km², extent, road km, motorway refs found,
building counts.

**Full county (what the game ships with):**

```bash
curl -L -o data/pbf/greater-manchester-latest.osm.pbf https://download.geofabrik.de/europe/united-kingdom/england/greater-manchester-latest.osm.pbf
node pbf_to_tiles.mjs
node fetch_terrain.mjs
node --max-old-space-size=10000 process_osm.mjs --install
```

About 5 minutes in total. `process_osm.mjs` keeps `terrain.bin` and replaces the cells.
Run `fetch_terrain.mjs` before processing: bridge deck heights come from it.
(`fetch_osm.mjs --all` still works but makes ~200 Overpass requests and takes hours.)

Licence: OSM data © OpenStreetMap contributors, ODbL 1.0. `data/raw` is a download
cache and isn't committed.
