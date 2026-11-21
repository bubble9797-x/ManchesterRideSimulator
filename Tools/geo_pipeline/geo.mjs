// Geographic helpers shared by the pipeline. The projection MUST stay identical to
// Source/ManchesterRide/Sim/GeoProjection.cpp (Transverse Mercator, WGS84, origin
// Manchester Town Hall). test_geo.mjs checks reference values against the C++ build.

export const ORIGIN = { lat: 53.47936, lon: -2.24484 };

// Greater Manchester extent (generous; the boundary polygon trims it).
// Covers Wigan (W) to Saddleworth/Mossley (E), Ramsbottom/Littleborough (N) to Hale/Marple (S).
export const GM_BBOX = { south: 53.33, west: -2.74, north: 53.69, east: -1.90 };

const A = 6378137.0;
const F = 1 / 298.257223563;
const E2 = F * (2 - F);
const EP2 = E2 / (1 - E2);
const DEG = Math.PI / 180;

function meridianArc(phi) {
  const e4 = E2 * E2, e6 = e4 * E2;
  return A * ((1 - E2 / 4 - 3 * e4 / 64 - 5 * e6 / 256) * phi
    - (3 * E2 / 8 + 3 * e4 / 32 + 45 * e6 / 1024) * Math.sin(2 * phi)
    + (15 * e4 / 256 + 45 * e6 / 1024) * Math.sin(4 * phi)
    - (35 * e6 / 3072) * Math.sin(6 * phi));
}

/** WGS84 lat/lon (deg) -> local east/north metres. */
export function toLocal(lat, lon, lat0 = ORIGIN.lat, lon0 = ORIGIN.lon) {
  const phi = lat * DEG, lam = lon * DEG, phi0 = lat0 * DEG, lam0 = lon0 * DEG;
  const s = Math.sin(phi), c = Math.cos(phi), t = Math.tan(phi);
  const N = A / Math.sqrt(1 - E2 * s * s);
  const T = t * t, C = EP2 * c * c, Aa = (lam - lam0) * c;
  const M = meridianArc(phi), M0 = meridianArc(phi0);
  const east = N * (Aa + (1 - T + C) * Aa ** 3 / 6 + (5 - 18 * T + T * T + 72 * C - 58 * EP2) * Aa ** 5 / 120);
  const north = (M - M0) + N * t * (Aa * Aa / 2 + (5 - T + 9 * C + 4 * C * C) * Aa ** 4 / 24
    + (61 - 58 * T + T * T + 600 * C - 330 * EP2) * Aa ** 6 / 720);
  return { east, north };
}

/** Local east/north metres -> WGS84 lat/lon (deg). */
export function toLatLon(east, north, lat0 = ORIGIN.lat, lon0 = ORIGIN.lon) {
  const phi0 = lat0 * DEG, lam0 = lon0 * DEG;
  const M = meridianArc(phi0) + north;
  const e4 = E2 * E2, e6 = e4 * E2;
  const mu = M / (A * (1 - E2 / 4 - 3 * e4 / 64 - 5 * e6 / 256));
  const e1 = (1 - Math.sqrt(1 - E2)) / (1 + Math.sqrt(1 - E2));
  const phi1 = mu + (3 * e1 / 2 - 27 * e1 ** 3 / 32) * Math.sin(2 * mu)
    + (21 * e1 * e1 / 16 - 55 * e1 ** 4 / 32) * Math.sin(4 * mu)
    + (151 * e1 ** 3 / 96) * Math.sin(6 * mu)
    + (1097 * e1 ** 4 / 512) * Math.sin(8 * mu);
  const s1 = Math.sin(phi1), c1 = Math.cos(phi1), t1 = Math.tan(phi1);
  const C1 = EP2 * c1 * c1, T1 = t1 * t1;
  const N1 = A / Math.sqrt(1 - E2 * s1 * s1);
  const R1 = A * (1 - E2) / Math.pow(1 - E2 * s1 * s1, 1.5);
  const D = east / N1;
  const lat = (phi1 - (N1 * t1 / R1) * (D * D / 2 - (5 + 3 * T1 + 10 * C1 - 4 * C1 * C1 - 9 * EP2) * D ** 4 / 24
    + (61 + 90 * T1 + 298 * C1 + 45 * T1 * T1 - 252 * EP2 - 3 * C1 * C1) * D ** 6 / 720)) / DEG;
  const lon = (lam0 + (D - (1 + 2 * T1 + C1) * D ** 3 / 6
    + (5 - 2 * C1 + 28 * T1 - 3 * C1 * C1 + 8 * EP2 + 24 * T1 * T1) * D ** 5 / 120) / c1) / DEG;
  return { lat, lon };
}

/** Local metres -> Unreal centimetres (+X east, +Y south). */
export const toUnreal = (east, north) => ({ x: east * 100, y: -north * 100 });

/** Great-circle distance (m), used only for validation. */
export function haversine(lat1, lon1, lat2, lon2) {
  const R = 6371008.8;
  const dLat = (lat2 - lat1) * DEG, dLon = (lon2 - lon1) * DEG;
  const a = Math.sin(dLat / 2) ** 2 + Math.cos(lat1 * DEG) * Math.cos(lat2 * DEG) * Math.sin(dLon / 2) ** 2;
  return 2 * R * Math.asin(Math.sqrt(a));
}

/** Vincenty inverse: ellipsoidal (WGS84) distance in metres - the ground truth for validation. */
export function vincenty(lat1, lon1, lat2, lon2) {
  const b = A * (1 - F);
  const L = (lon2 - lon1) * DEG;
  const U1 = Math.atan((1 - F) * Math.tan(lat1 * DEG)), U2 = Math.atan((1 - F) * Math.tan(lat2 * DEG));
  const sU1 = Math.sin(U1), cU1 = Math.cos(U1), sU2 = Math.sin(U2), cU2 = Math.cos(U2);
  let lam = L, prev, sS, cS, sig, cA2, c2m;
  for (let i = 0; i < 200; i++) {
    const sl = Math.sin(lam), cl = Math.cos(lam);
    sS = Math.sqrt((cU2 * sl) ** 2 + (cU1 * sU2 - sU1 * cU2 * cl) ** 2);
    if (sS === 0) return 0;
    cS = sU1 * sU2 + cU1 * cU2 * cl;
    sig = Math.atan2(sS, cS);
    const sA = cU1 * cU2 * sl / sS;
    cA2 = 1 - sA * sA;
    c2m = cA2 ? cS - 2 * sU1 * sU2 / cA2 : 0;
    const C = F / 16 * cA2 * (4 + F * (4 - 3 * cA2));
    prev = lam;
    lam = L + (1 - C) * F * sA * (sig + C * sS * (c2m + C * cS * (-1 + 2 * c2m * c2m)));
    if (Math.abs(lam - prev) < 1e-12) break;
  }
  const u2 = cA2 * (A * A - b * b) / (b * b);
  const Aa = 1 + u2 / 16384 * (4096 + u2 * (-768 + u2 * (320 - 175 * u2)));
  const Bb = u2 / 1024 * (256 + u2 * (-128 + u2 * (74 - 47 * u2)));
  const dS = Bb * sS * (c2m + Bb / 4 * (cS * (-1 + 2 * c2m * c2m) - Bb / 6 * c2m * (-3 + 4 * sS * sS) * (-3 + 4 * c2m * c2m)));
  return b * Aa * (sig - dS);
}

/** Split a lat/lon bbox into download tiles of roughly `sizeM` metres. */
export function tileBBox(bbox, sizeM) {
  const midLat = (bbox.south + bbox.north) / 2;
  const dLat = sizeM / 111320;
  const dLon = sizeM / (111320 * Math.cos(midLat * DEG));
  const tiles = [];
  let iy = 0;
  for (let s = bbox.south; s < bbox.north; s += dLat, iy++) {
    let ix = 0;
    for (let w = bbox.west; w < bbox.east; w += dLon, ix++) {
      tiles.push({ ix, iy, south: s, west: w, north: Math.min(s + dLat, bbox.north), east: Math.min(w + dLon, bbox.east) });
    }
  }
  return tiles;
}

/** Point-in-polygon (ray casting) on [lon, lat] rings. */
export function pointInRings(lon, lat, rings) {
  let inside = false;
  for (const ring of rings) {
    for (let i = 0, j = ring.length - 1; i < ring.length; j = i++) {
      const [xi, yi] = ring[i], [xj, yj] = ring[j];
      if ((yi > lat) !== (yj > lat) && lon < ((xj - xi) * (lat - yi)) / (yj - yi) + xi) inside = !inside;
    }
  }
  return inside;
}
