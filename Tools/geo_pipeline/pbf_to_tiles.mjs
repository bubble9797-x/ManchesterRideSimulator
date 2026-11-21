#!/usr/bin/env node
// Full-county import, fast route: read a Geofabrik .osm.pbf extract and write the same
// Overpass-style element files that fetch_osm.mjs produces (data/raw/tile_pbf_*.json),
// so process_osm.mjs handles both sources identically.
//
//   curl -L -o data/pbf/greater-manchester-latest.osm.pbf \
//     https://download.geofabrik.de/europe/united-kingdom/england/greater-manchester-latest.osm.pbf
//   node pbf_to_tiles.mjs [data/pbf/greater-manchester-latest.osm.pbf]
//
// No npm packages: a minimal protobuf decoder + node:zlib. Handles DenseNodes, Ways and
// multipolygon Relations (outer rings stitched; inner rings/holes are dropped for now).
// Feature filter is identical to fetch_osm.mjs's Overpass query.

import { readFileSync, writeFileSync, mkdirSync, readdirSync, rmSync } from 'node:fs';
import { inflateSync } from 'node:zlib';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const RAW = join(HERE, 'data', 'raw');
const PBF = process.argv[2] || join(HERE, 'data', 'pbf', 'greater-manchester-latest.osm.pbf');

// ------------------------------------------------------------------ protobuf

class Reader {
  constructor(buf, pos = 0, end = buf.length) { this.buf = buf; this.pos = pos; this.end = end; }
  eof() { return this.pos >= this.end; }
  varint() { // up to 64-bit, returned as a JS number (exact below 2^53, which covers OSM ids)
    let result = 0, mul = 1, b;
    do { b = this.buf[this.pos++]; result += (b & 0x7f) * mul; mul *= 128; } while (b & 0x80);
    return result;
  }
  svarint() { const v = this.varint(); return v % 2 === 0 ? v / 2 : -(v + 1) / 2; }
  bytes() { const len = this.varint(); const s = this.pos; this.pos += len; return this.buf.subarray(s, s + len); }
  skip(wire) {
    if (wire === 0) this.varint();
    else if (wire === 1) this.pos += 8;
    else if (wire === 2) { const len = this.varint(); this.pos += len; } // read len first: `pos += varint()` would use the stale pos
    else if (wire === 5) this.pos += 4;
    else throw new Error(`unsupported wire type ${wire}`);
  }
  // Iterate fields: cb is called with `this` = reader and (fieldNumber, wireType); it must
  // consume the value, or return false to have it skipped.
  fields(cb) {
    while (!this.eof()) {
      const tag = this.varint(); const f = Math.floor(tag / 8), w = tag & 7;
      if (cb.call(this, f, w) === false) this.skip(w);
    }
  }
}
const packed = (buf, signed) => { const r = new Reader(buf); const out = []; while (!r.eof()) out.push(signed ? r.svarint() : r.varint()); return out; };

// ------------------------------------------------------------------ feature filter (= fetch_osm.mjs)

const RE = {
  natural: /^(wood|scrub|grassland|heath|water|wetland|bare_rock)$/,
  leisure: /^(park|pitch|golf_course|recreation_ground|nature_reserve|garden|track)$/,
  waterway: /^(river|canal|stream)$/, railway: /^(rail|light_rail|tram)$/,
  nodeHighway: /^(traffic_signals|bus_stop|crossing|street_lamp|give_way|stop)$/,
  nodeAmenity: /^(post_box|bench|bicycle_parking|telephone)$/, place: /^(city|town|suburb|village|neighbourhood|quarter)$/,
};
const wantWay = (t) => !!(t.highway || t.building || t.landuse || RE.natural.test(t.natural || '') || RE.leisure.test(t.leisure || '')
  || RE.waterway.test(t.waterway || '') || RE.railway.test(t.railway || '') || t.amenity === 'parking');
const wantArea = (t) => !!(t.building || t.landuse || RE.natural.test(t.natural || '') || RE.leisure.test(t.leisure || '') || t.amenity === 'parking');
const wantNode = (t) => RE.nodeHighway.test(t.highway || '') || RE.nodeAmenity.test(t.amenity || '') || RE.place.test(t.place || '');

// ------------------------------------------------------------------ blocks

function* blocks(file) {
  let pos = 0;
  while (pos < file.length) {
    const hlen = file.readUInt32BE(pos); pos += 4;
    let type = '', datasize = 0;
    new Reader(file, pos, pos + hlen).fields(function (f, w) {
      if (f === 1) { type = this.bytes().toString(); return; }
      if (f === 3) { datasize = this.varint(); return; }
      return false;
    });
    pos += hlen;
    const blobStart = pos; pos += datasize;
    if (type !== 'OSMData') continue;
    let raw = null, zlib = null;
    new Reader(file, blobStart, blobStart + datasize).fields(function (f) {
      if (f === 1) { raw = this.bytes(); return; }
      if (f === 3) { zlib = this.bytes(); return; }
      return false;
    });
    if (!raw && !zlib) throw new Error('unsupported blob compression (only zlib/raw)');
    yield raw ?? inflateSync(zlib);
  }
}

function decodeBlock(buf, handlers) {
  const r = new Reader(buf);
  let strings = [], groups = [], gran = 100, latOff = 0, lonOff = 0;
  r.fields(function (f) {
    if (f === 1) { const st = new Reader(this.bytes()); st.fields(function (g) { if (g === 1) { strings.push(this.bytes().toString('utf8')); return; } return false; }); return; }
    if (f === 2) { groups.push(this.bytes()); return; }
    if (f === 17) { gran = this.varint(); return; }
    if (f === 19) { latOff = this.varint(); return; }   // int64 (non-zigzag); 0 in Geofabrik extracts
    if (f === 20) { lonOff = this.varint(); return; }
    return false;
  });
  const tagsOf = (keys, vals) => { const t = {}; for (let i = 0; i < keys.length; i++) t[strings[keys[i]]] = strings[vals[i]]; return t; };
  for (const g of groups) {
    new Reader(g).fields(function (f) {
      if (f === 2 && handlers.dense) { // DenseNodes
        let ids = [], lats = [], lons = [], kv = [];
        new Reader(this.bytes()).fields(function (d) {
          if (d === 1) { ids = packed(this.bytes(), true); return; }
          if (d === 8) { lats = packed(this.bytes(), true); return; }
          if (d === 9) { lons = packed(this.bytes(), true); return; }
          if (d === 10) { kv = packed(this.bytes(), false); return; }
          return false;
        });
        let id = 0, la = 0, lo = 0, k = 0;
        for (let i = 0; i < ids.length; i++) {
          id += ids[i]; la += lats[i]; lo += lons[i];
          let tags = null;
          if (kv.length) { while (kv[k] !== 0) { (tags ??= {})[strings[kv[k]]] = strings[kv[k + 1]]; k += 2; } k++; }
          handlers.dense(id, 1e-9 * (latOff + gran * la), 1e-9 * (lonOff + gran * lo), tags);
        }
        return;
      }
      if (f === 3 && handlers.way) {
        let id = 0, keys = [], vals = [], refs = [];
        new Reader(this.bytes()).fields(function (d) {
          if (d === 1) { id = this.varint(); return; }
          if (d === 2) { keys = packed(this.bytes(), false); return; }
          if (d === 3) { vals = packed(this.bytes(), false); return; }
          if (d === 8) { refs = packed(this.bytes(), true); return; }
          return false;
        });
        for (let i = 1; i < refs.length; i++) refs[i] += refs[i - 1];
        handlers.way(id, tagsOf(keys, vals), refs);
        return;
      }
      if (f === 4 && handlers.relation) {
        let id = 0, keys = [], vals = [], roles = [], mem = [], types = [];
        new Reader(this.bytes()).fields(function (d) {
          if (d === 1) { id = this.varint(); return; }
          if (d === 2) { keys = packed(this.bytes(), false); return; }
          if (d === 3) { vals = packed(this.bytes(), false); return; }
          if (d === 8) { roles = packed(this.bytes(), false); return; }
          if (d === 9) { mem = packed(this.bytes(), true); return; }
          if (d === 10) { types = packed(this.bytes(), false); return; }
          return false;
        });
        for (let i = 1; i < mem.length; i++) mem[i] += mem[i - 1];
        handlers.relation(id, tagsOf(keys, vals), mem.map((ref, i) => ({ ref, type: types[i], role: strings[roles[i]] })));
        return;
      }
      if (f === 1 && handlers.dense) { // plain Node (rare in extracts)
        let id = 0, keys = [], vals = [], la = 0, lo = 0;
        new Reader(this.bytes()).fields(function (d) {
          if (d === 1) { id = this.svarint(); return; }
          if (d === 2) { keys = packed(this.bytes(), false); return; }
          if (d === 3) { vals = packed(this.bytes(), false); return; }
          if (d === 8) { la = this.svarint(); return; }
          if (d === 9) { lo = this.svarint(); return; }
          return false;
        });
        handlers.dense(id, 1e-9 * (latOff + gran * la), 1e-9 * (lonOff + gran * lo), keys.length ? tagsOf(keys, vals) : null);
        return;
      }
      return false;
    });
  }
}

// ------------------------------------------------------------------ node store (sorted ids -> coords)

class NodeStore {
  constructor() { this.n = 0; this.cap = 1 << 22; this.ids = new Float64Array(this.cap); this.lat = new Float64Array(this.cap); this.lon = new Float64Array(this.cap); this.sorted = true; }
  add(id, la, lo) {
    if (this.n === this.cap) { this.cap *= 2; for (const k of ['ids', 'lat', 'lon']) { const a = new Float64Array(this.cap); a.set(this[k]); this[k] = a; } }
    if (this.n && id < this.ids[this.n - 1]) this.sorted = false;
    this.ids[this.n] = id; this.lat[this.n] = la; this.lon[this.n] = lo; this.n++;
  }
  finish() {
    if (this.sorted) return;
    const order = Array.from({ length: this.n }, (_, i) => i).sort((a, b) => this.ids[a] - this.ids[b]);
    for (const k of ['ids', 'lat', 'lon']) { const src = this[k], a = new Float64Array(this.n); order.forEach((o, i) => { a[i] = src[o]; }); this[k] = a; }
  }
  get(id) {
    let lo = 0, hi = this.n - 1;
    while (lo <= hi) { const m = (lo + hi) >> 1; const v = this.ids[m]; if (v === id) return m; if (v < id) lo = m + 1; else hi = m - 1; }
    return -1;
  }
}

// ------------------------------------------------------------------ main

const t0 = Date.now();
const file = readFileSync(PBF);
console.log(`Reading ${PBF} (${(file.length / 1e6).toFixed(1)} MB)`);

// Pass 1: multipolygon relations we want, and the member ways they need.
const relations = [];
const memberWays = new Set();
for (const b of blocks(file)) decodeBlock(b, {
  relation(id, tags, members) {
    if (tags.type !== 'multipolygon' || !wantArea(tags)) return;
    relations.push({ id, tags, members: members.filter((m) => m.type === 1) });
    for (const m of members) if (m.type === 1) memberWays.add(m.ref);
  },
});
console.log(`  pass 1: ${relations.length} multipolygon relations, ${memberWays.size} member ways (${((Date.now() - t0) / 1000).toFixed(1)} s)`);

// Pass 2: nodes, ways.
const nodes = new NodeStore();
const taggedNodes = [];
const ways = [];
const memberGeom = new Map();
for (const b of blocks(file)) decodeBlock(b, {
  dense(id, la, lo, tags) { nodes.add(id, la, lo); if (tags && wantNode(tags)) taggedNodes.push({ type: 'node', id, lat: +la.toFixed(7), lon: +lo.toFixed(7), tags }); },
  way(id, tags, refs) {
    const want = wantWay(tags), member = memberWays.has(id);
    if (want || member) ways.push({ id, tags, refs, want, member });
  },
});
nodes.finish();
console.log(`  pass 2: ${nodes.n} nodes, ${taggedNodes.length} tagged nodes, ${ways.length} ways (${((Date.now() - t0) / 1000).toFixed(1)} s)`);

let missing = 0;
const geomOf = (refs) => {
  const g = [];
  for (const r of refs) { const i = nodes.get(r); if (i < 0) { missing++; continue; } g.push({ lat: +nodes.lat[i].toFixed(7), lon: +nodes.lon[i].toFixed(7) }); }
  return g;
};

// Buckets of ~0.05 deg so no single output file is huge.
const buckets = new Map();
const bucketOf = (lat, lon) => `${Math.floor(lon / 0.06)}_${Math.floor(lat / 0.04)}`;
const push = (el, lat, lon) => { const k = bucketOf(lat, lon); if (!buckets.has(k)) buckets.set(k, []); buckets.get(k).push(el); };

for (const n of taggedNodes) push(n, n.lat, n.lon);
for (const w of ways) {
  const g = geomOf(w.refs);
  if (w.member) memberGeom.set(w.id, { refs: w.refs, g });
  if (!w.want || g.length < 2) continue;
  const mid = g[g.length >> 1];
  push({ type: 'way', id: w.id, tags: w.tags, geometry: g }, mid.lat, mid.lon);
}

// Stitch relation outer members into closed rings; each ring becomes a synthetic closed way (negative id).
let relRings = 0;
for (const rel of relations) {
  const segs = rel.members.filter((m) => m.role !== 'inner' && memberGeom.has(m.ref)).map((m) => memberGeom.get(m.ref).refs.slice());
  const rings = [];
  while (segs.length) {
    let ring = segs.shift(), grew = true;
    while (grew && ring[0] !== ring.at(-1)) {
      grew = false;
      for (let i = 0; i < segs.length; i++) {
        const s = segs[i], end = ring.at(-1);
        if (s[0] === end) ring = ring.concat(s.slice(1));
        else if (s.at(-1) === end) ring = ring.concat(s.slice(0, -1).reverse());
        else if (s.at(-1) === ring[0]) ring = s.slice(0, -1).concat(ring);
        else if (s[0] === ring[0]) ring = s.slice(1).reverse().concat(ring);
        else continue;
        segs.splice(i, 1); grew = true; break;
      }
    }
    if (ring.length >= 4 && ring[0] === ring.at(-1)) rings.push(ring);
  }
  rings.forEach((ring, idx) => {
    const g = geomOf(ring);
    if (g.length < 4) return;
    const mid = g[g.length >> 1];
    push({ type: 'way', id: -(rel.id * 10 + idx), tags: { ...rel.tags, type: undefined }, geometry: g }, mid.lat, mid.lon);
    relRings++;
  });
}

mkdirSync(RAW, { recursive: true });
for (const f of readdirSync(RAW)) if (f.startsWith('tile_pbf_')) rmSync(join(RAW, f));
let total = 0;
for (const [k, els] of buckets) { writeFileSync(join(RAW, `tile_pbf_${k}.json`), JSON.stringify({ source: 'geofabrik-pbf', elements: els })); total += els.length; }
console.log(`  wrote ${buckets.size} files, ${total} elements (${relRings} rings from relations), ${missing} missing node refs, ${((Date.now() - t0) / 1000).toFixed(1)} s`);
