// Reader for Content/GeoData/terrain.bin (format documented in fetch_terrain.mjs).
import { readFileSync, existsSync } from 'node:fs';

/** Returns { heightCm(x, y) } in Unreal cm relative to the origin height, or null if the file is missing. */
export function loadTerrain(path) {
  if (!existsSync(path)) return null;
  const buf = readFileSync(path);
  if (buf.toString('latin1', 0, 4) !== 'MRTR') throw new Error(`${path}: bad magic`);
  const minX = buf.readInt32LE(8), minY = buf.readInt32LE(12), sp = buf.readUInt32LE(16), nx = buf.readUInt32LE(20), ny = buf.readUInt32LE(24);
  const baseM = buf.readFloatLE(28);
  const h = new Int16Array(buf.buffer.slice(buf.byteOffset + 32, buf.byteOffset + 32 + nx * ny * 2));
  const at = (x, y) => h[Math.min(ny - 1, Math.max(0, y)) * nx + Math.min(nx - 1, Math.max(0, x))] * 10; // dm -> cm
  return {
    baseM,
    /** Bilinear height (cm) at Unreal X/Y (cm). Same as FMRTerrain::SampleRaw in the game. */
    heightCm(x, y) {
      const gx = (x - minX) / sp, gy = (y - minY) / sp;
      const x0 = Math.floor(gx), y0 = Math.floor(gy), fx = gx - x0, fy = gy - y0;
      return (at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) + (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy;
    },
  };
}
