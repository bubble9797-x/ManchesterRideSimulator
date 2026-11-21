#!/usr/bin/env node
// Validates the projection used by both the pipeline and the game.
//   node test_geo.mjs
import { toLocal, toLatLon, vincenty, ORIGIN } from './geo.mjs';

let failures = 0;
const check = (ok, msg) => { if (!ok) { failures++; console.log('  FAIL ' + msg); } else console.log('  ok   ' + msg); };

// Reference places (approximate centres) across all ten boroughs.
const PLACES = {
  'Manchester Town Hall': [53.47936, -2.24484], 'Piccadilly Gardens': [53.48099, -2.23697], 'Salford Quays (MediaCityUK)': [53.47250, -2.29780],
  'Stockport town centre': [53.40990, -2.15760], 'Ashton-under-Lyne': [53.48890, -2.09820], 'Oldham town centre': [53.54140, -2.11690],
  'Rochdale town centre': [53.61610, -2.15480], 'Bury town centre': [53.59330, -2.29660], 'Bolton town centre': [53.57800, -2.42940],
  'Wigan town centre': [53.54490, -2.63130], 'Altrincham': [53.38730, -2.34880], 'Littleborough': [53.64400, -2.09400],
};

console.log('[origin]');
const o = toLocal(ORIGIN.lat, ORIGIN.lon);
check(Math.abs(o.east) < 1e-6 && Math.abs(o.north) < 1e-6, 'origin maps to (0,0)');

console.log('[round trip < 1 mm]');
for (const [name, [lat, lon]] of Object.entries(PLACES)) {
  const l = toLocal(lat, lon);
  const back = toLatLon(l.east, l.north);
  const errM = vincenty(lat, lon, back.lat, back.lon);
  check(errM < 0.001, `${name}: ${(l.east / 1000).toFixed(2)} km E, ${(l.north / 1000).toFixed(2)} km N, round-trip error ${(errM * 1000).toFixed(4)} mm`);
}

console.log('[distances match WGS84 ellipsoidal (Vincenty) within 0.01 %]');
const names = Object.keys(PLACES);
for (let i = 1; i < names.length; i++) {
  const [la1, lo1] = PLACES[names[0]], [la2, lo2] = PLACES[names[i]];
  const a = toLocal(la1, lo1), b = toLocal(la2, lo2);
  const planar = Math.hypot(a.east - b.east, a.north - b.north);
  const gc = vincenty(la1, lo1, la2, lo2);
  const err = Math.abs(planar - gc) / gc * 100;
  check(err < 0.01, `Town Hall -> ${names[i]}: ${(gc / 1000).toFixed(2)} km (planar error ${err.toFixed(4)} %)`);
}

console.log('[directions]');
const wigan = toLocal(...PLACES['Wigan town centre']), stockport = toLocal(...PLACES['Stockport town centre']), rochdale = toLocal(...PLACES['Rochdale town centre']);
check(wigan.east < -20000, 'Wigan is >20 km west of the origin');
check(stockport.north < -5000 && stockport.east > 0, 'Stockport is south-east');
check(rochdale.north > 10000, 'Rochdale is >10 km north');

// Values the C++ test compares against (Tests/SimTests "geo" checks).
const pg = toLocal(...PLACES['Piccadilly Gardens']);
console.log(`\nC++ cross-check reference: Piccadilly Gardens east=${pg.east.toFixed(4)} north=${pg.north.toFixed(4)}`);
console.log(failures ? `\n${failures} failure(s)` : '\nall geo checks passed');
process.exit(failures ? 1 : 0);
