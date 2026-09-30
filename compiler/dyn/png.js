// luacretro dyn - minimal PNG decoder for .p8.png carts. Supports exactly what
// cart files use: 8-bit RGBA or RGB, non-interlaced, filter types 0-4. Anything
// else is rejected with a clear error rather than decoded wrongly.

import { inflateZlib } from "./inflate.js";

const SIG = [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a];

/** @param {Uint8Array} b */
export function isPng(b) {
  return b.length >= 8 && SIG.every((v, i) => b[i] === v);
}

const u32 = (b, o) => ((b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]) >>> 0;

function paeth(a, b, c) {
  const p = a + b - c;
  const pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c);
  return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

/**
 * Decode a PNG to RGBA8 (RGB images get alpha 255).
 * @param {Uint8Array} bytes
 * @returns {{width:number, height:number, rgba:Uint8Array}}
 */
export function decodePng(bytes) {
  if (!isPng(bytes)) throw new Error("png: missing PNG signature");
  let pos = 8, ihdr = null;
  const idat = [];
  let idatLen = 0;
  for (;;) {
    if (pos + 8 > bytes.length) throw new Error("png: truncated before IEND");
    const len = u32(bytes, pos);
    const type = String.fromCharCode(bytes[pos + 4], bytes[pos + 5], bytes[pos + 6], bytes[pos + 7]);
    const data = bytes.subarray(pos + 8, pos + 8 + len);
    if (data.length !== len) throw new Error(`png: truncated ${type} chunk`);
    pos += 12 + len;   // length + type + data + CRC
    if (type === "IHDR") {
      ihdr = {
        width: u32(data, 0), height: u32(data, 4), depth: data[8], color: data[9],
        compression: data[10], filter: data[11], interlace: data[12],
      };
    } else if (type === "IDAT") {
      idat.push(data); idatLen += len;
    } else if (type === "IEND") {
      break;
    } else if (type[0] >= "A" && type[0] <= "Z" && type !== "PLTE") {
      // uppercase first letter = critical chunk we do not understand
      throw new Error(`png: unsupported critical chunk ${type}`);
    }
  }
  if (!ihdr) throw new Error("png: missing IHDR");
  const { width, height, depth, color, interlace } = ihdr;
  if (depth !== 8) throw new Error(`png: unsupported bit depth ${depth} (only 8-bit supported)`);
  if (color !== 6 && color !== 2) {
    throw new Error(`png: unsupported color type ${color} (only RGBA=6 and RGB=2 supported)`);
  }
  if (interlace !== 0) throw new Error("png: interlaced images are not supported");
  if (ihdr.compression !== 0 || ihdr.filter !== 0) throw new Error("png: unknown compression/filter method");
  if (!idat.length) throw new Error("png: no IDAT data");

  const z = new Uint8Array(idatLen);
  let o = 0;
  for (const d of idat) { z.set(d, o); o += d.length; }
  const raw = inflateZlib(z);

  const bpp = color === 6 ? 4 : 3;
  const stride = width * bpp;
  if (raw.length < height * (stride + 1)) throw new Error("png: image data is shorter than IHDR says");

  // unfilter in place into `px`, one scanline at a time
  const px = new Uint8Array(height * stride);
  for (let y = 0; y < height; y++) {
    const ft = raw[y * (stride + 1)];
    const src = y * (stride + 1) + 1, dst = y * stride, up = dst - stride;
    for (let x = 0; x < stride; x++) {
      const a = x >= bpp ? px[dst + x - bpp] : 0;
      const b = y > 0 ? px[up + x] : 0;
      const c = x >= bpp && y > 0 ? px[up + x - bpp] : 0;
      let v = raw[src + x];
      switch (ft) {
        case 0: break;
        case 1: v += a; break;
        case 2: v += b; break;
        case 3: v += (a + b) >> 1; break;
        case 4: v += paeth(a, b, c); break;
        default: throw new Error(`png: invalid filter type ${ft} on row ${y}`);
      }
      px[dst + x] = v;   // Uint8Array store wraps mod 256
    }
  }

  if (bpp === 4) return { width, height, rgba: px };
  const rgba = new Uint8Array(width * height * 4);
  for (let i = 0, j = 0; i < px.length; i += 3, j += 4) {
    rgba[j] = px[i]; rgba[j + 1] = px[i + 1]; rgba[j + 2] = px[i + 2]; rgba[j + 3] = 255;
  }
  return { width, height, rgba };
}
