// luacretro dyn - PICO-8 cart loader. Turns a cart file into its Lua source
// and the 0x4300-byte cart ROM image in PICO-8's in-memory layout:
//
//   0x0000 sprite sheet (0x1000-0x1fff shared with the map's lower half)
//   0x2000 map (upper 32 rows)      0x3000 sprite flags
//   0x3100 music (64 x 4 bytes)     0x3200 sfx (64 x 68 bytes)
//
// Formats: .p8 text carts and .p8.png carts (code raw, legacy ':c:' or 'PXA'
// compressed). Browser-safe: plain Uint8Array, own PNG + inflate, no node:.
// The reference is FAKE-08's source/cart.cpp; deliberate differences are
// called out inline.

import { decodePng, isPng } from "./png.js";

export const ROM_SIZE = 0x4300;
const GFX = 0x0000, MAP = 0x2000, GFF = 0x3000, MUSIC = 0x3100, SFX = 0x3200;
const CODE = 0x4300, CODE_END = 0x8000;
const EMPTY_PATTERN = [0x41, 0x42, 0x43, 0x44];

/**
 * @typedef {{path:string, line:number, tab?:number}} CartInclude
 * @typedef {{
 *   lua: string,
 *   luaEncoding: "unicode" | "p8scii",
 *   rom: Uint8Array,
 *   label?: Uint8Array,
 *   version?: number,
 *   codeFormat?: "text" | "raw" | "legacy" | "pxa",
 *   includes?: CartInclude[],
 * }} Cart
 * @typedef {{
 *   resolveInclude?: (path: string, from: string) => string | Uint8Array | null | undefined,
 * }} CartOptions
 */

/**
 * Load a cart from its file bytes. The format comes from the content (PNG
 * signature or the "pico-8 cartridge" header), not the name; `name` labels
 * errors and is passed to `resolveInclude` as the including file.
 * @param {Uint8Array} bytes
 * @param {string} name
 * @param {CartOptions} [options]
 * @returns {Cart}
 */
export function loadCart(bytes, name, options = {}) {
  if (!(bytes instanceof Uint8Array)) throw new TypeError(`${name}: loadCart expects a Uint8Array`);
  if (bytes.length === 0) throw new Error(`${name}: cart file is empty`);
  if (isPng(bytes)) {
    try { return decodeP8Png(bytes); } catch (e) { throw new Error(`${name}: ${e.message}`); }
  }
  const text = utf8(bytes);
  if (/^\s*pico-8 cartridge/.test(text)) return parseP8Text(text, { ...options, name });
  const head = text.slice(0, 40).replace(/\s+/g, " ").trim();
  throw new Error(`${name}: not a PICO-8 cart (no PNG signature or "pico-8 cartridge" header; starts with "${head}")`);
}

/** @param {Uint8Array} bytes */
function utf8(bytes) {
  const s = new TextDecoder("utf-8").decode(bytes);
  return s.charCodeAt(0) === 0xfeff ? s.slice(1) : s;
}

// ---------------------------------------------------------------------------
// .p8 text carts

const SECTION = /^__([a-z0-9]+)__\s*$/;
const INCLUDE = /^\s*#include\s+(\S+)\s*$/;

/** Hex digit value, 0 for anything that is not a hex digit (strtol-ish). */
function hex(ch) {
  const c = ch === undefined ? 0 : ch.charCodeAt(0);
  if (c >= 48 && c <= 57) return c - 48;
  if (c >= 97 && c <= 102) return c - 87;
  if (c >= 65 && c <= 70) return c - 55;
  return 0;
}
const hex2 = (s, i) => (hex(s[i]) << 4) | hex(s[i + 1]);

/** Non-whitespace chars of a section, as one hex digit stream. */
const digits = (lines) => lines.join("").replace(/\s+/g, "");

/**
 * Parse a .p8 text cart. The Lua comes back exactly as written (unicode; the
 * newline that ends the last code line before the next section header is the
 * file format's, not the code's, so it matches what a .p8.png of the same cart
 * stores). P8SCII conversion is the caller's step.
 * @param {string} text
 * @param {CartOptions & {name?: string}} [options]
 * @returns {Cart}
 */
export function parseP8Text(text, options = {}) {
  const name = options.name ?? "<cart>";
  const lines = text.split("\n").map((l) => (l.endsWith("\r") ? l.slice(0, -1) : l));
  if (lines.length && lines[lines.length - 1] === "") lines.pop();   // the file's final newline
  /** @type {Record<string, {start:number, lines:string[]}>} */
  const sections = {};
  let version, cur = null;
  lines.forEach((line, i) => {
    const m = SECTION.exec(line);
    if (m) { cur = sections[m[1]] ??= { start: i + 2, lines: [] }; return; }
    if (cur) { cur.lines.push(line); return; }
    const v = /^version\s+(\d+)/.exec(line);
    if (v) version = Number(v[1]);
  });

  const rom = new Uint8Array(ROM_SIZE);

  // gfx: 128 rows of 128 pixels; each byte holds two pixels, LEFT pixel in the
  // low nibble, so a text pair "lr" stores as 0xRL.
  if (sections.gfx) {
    const d = digits(sections.gfx.lines);
    for (let i = 0, o = GFX; i + 1 < d.length && o < MAP; i += 2) rom[o++] = hex(d[i]) | (hex(d[i + 1]) << 4);
  }
  if (sections.gff) {
    const d = digits(sections.gff.lines);
    for (let i = 0, o = GFF; i + 1 < d.length && o < MUSIC; i += 2) rom[o++] = hex2(d, i);
  }
  if (sections.map) {
    // only the upper 32 rows live here; rows 32-63 are the gfx lower half
    const d = digits(sections.map.lines);
    for (let i = 0, o = MAP; i + 1 < d.length && o < GFF; i += 2) rom[o++] = hex2(d, i);
  }
  writeSfx(rom, sections.sfx?.lines ?? []);
  writeMusic(rom, sections.music?.lines ?? []);

  /** @type {Cart} */
  const cart = { lua: "", luaEncoding: "unicode", rom, version, codeFormat: "text" };
  if (sections.label) cart.label = parseLabel(sections.label.lines);
  if (sections.lua) {
    const { lua, includes } = expandIncludes(sections.lua, name, options);
    cart.lua = lua;
    if (includes.length) cart.includes = includes;
  }
  return cart;
}

/**
 * Sfx text: one line per sfx, "FFSSLLEE" (editor/filter byte, speed, loop
 * start, loop end) then 32 notes of 5 hex chars: 2 pitch, 1 waveform (8-f =
 * custom instrument 0-7), 1 volume, 1 effect. Memory: notes as 16-bit LE words
 * pitch:6 wave:3 vol:3 fx:3 custom:1, then the four header bytes.
 * Every sfx defaults to speed 16 (as FAKE-08 does). Unlike FAKE-08, blank
 * lines are skipped instead of consuming an sfx slot as all-zero.
 */
function writeSfx(rom, lines) {
  for (let n = 0; n < 64; n++) rom[SFX + n * 68 + 65] = 16;
  let n = 0;
  for (const line of lines) {
    if (!line.trim()) continue;
    if (n >= 64) break;
    const base = SFX + n * 68;
    for (let k = 0; k < 32; k++) {
      const i = 8 + k * 5;
      const pitch = hex2(line, i), wave = hex(line[i + 2]);
      const vol = hex(line[i + 3]), fx = hex(line[i + 4]);
      rom[base + k * 2] = (pitch & 0x3f) | ((wave & 3) << 6);
      rom[base + k * 2 + 1] = ((wave >> 2) & 1) | ((vol & 7) << 1) | ((fx & 7) << 4) | (wave > 7 ? 0x80 : 0);
    }
    rom[base + 64] = hex2(line, 0);
    rom[base + 65] = hex2(line, 2);
    rom[base + 66] = hex2(line, 4);
    rom[base + 67] = hex2(line, 6);
    n++;
  }
}

/**
 * Music text: "FF AABBCCDD" - a flag byte (1 loop-begin, 2 loop-end, 4 stop,
 * 8 mode) and four channel bytes; the flags ride in bit 7 of channels 0-3.
 * Patterns the text omits are PICO-8's empty pattern 41 42 43 44 (every
 * channel disabled), as every PICO-8-saved .p8.png stores them. FAKE-08 leaves
 * them 0, which reads as "all four channels play sfx 0".
 */
function writeMusic(rom, lines) {
  for (let o = MUSIC; o < SFX; o += 4) rom.set(EMPTY_PATTERN, o);
  let n = 0;
  for (const line of lines) {
    if (line.length < 11) continue;
    if (n >= 64) break;
    const f = hex2(line, 0), o = MUSIC + n * 4;
    rom[o] = hex2(line, 3) | ((f & 1) << 7);
    rom[o + 1] = hex2(line, 5) | (((f >> 1) & 1) << 7);
    rom[o + 2] = hex2(line, 7) | (((f >> 2) & 1) << 7);
    rom[o + 3] = hex2(line, 9) | (((f >> 3) & 1) << 7);
    n++;
  }
}

/** Label: 128 rows of 128 chars, 0-9a-v = colors 0-31 (16-31 = the secret palette). */
function parseLabel(lines) {
  const label = new Uint8Array(128 * 128);
  for (let y = 0; y < 128 && y < lines.length; y++) {
    const row = lines[y];
    for (let x = 0; x < 128 && x < row.length; x++) {
      const v = parseInt(row[x], 32);
      label[y * 128 + x] = Number.isNaN(v) ? 0 : v;
    }
  }
  return label;
}

/**
 * Join the __lua__ lines, replacing each `#include path` line with the
 * resolved file. `path` may be a .lua/.txt file (used whole) or a .p8 cart,
 * optionally `cart.p8:N` for one code tab. Unresolvable includes are an error:
 * silently dropping code would compile a different game.
 */
function expandIncludes(section, name, options) {
  /** @type {CartInclude[]} */
  const includes = [];
  const out = [];
  section.lines.forEach((line, i) => {
    const m = INCLUDE.exec(line);
    if (!m) { out.push(line); return; }
    const lineNo = section.start + i;
    let path = m[1], tab;
    const t = /^(.*\.p8):(\d+)$/.exec(path);
    if (t) { path = t[1]; tab = Number(t[2]); }
    const got = options.resolveInclude?.(path, name);
    if (got == null) {
      throw new Error(`${name}:${lineNo}: cannot resolve "#include ${m[1]}"` +
        (options.resolveInclude ? " (resolveInclude returned nothing)" : " (no resolveInclude option given)"));
    }
    let src = typeof got === "string" ? got : utf8(got);
    if (/\.p8$/i.test(path)) {
      const inner = parseP8Text(src, { ...options, name: path });
      src = inner.lua;
      if (tab !== undefined) {
        const tabs = src.split(/\r?\n-->8\r?\n/);
        if (tab >= tabs.length) throw new Error(`${name}:${lineNo}: ${path} has no code tab ${tab}`);
        src = tabs[tab];
      }
    }
    src = src.replace(/\r\n/g, "\n").replace(/\n$/, "");
    includes.push(tab === undefined ? { path, line: lineNo } : { path, line: lineNo, tab });
    out.push(src);
  });
  return { lua: out.join("\n"), includes };
}

// ---------------------------------------------------------------------------
// .p8.png carts

/**
 * Decode a .p8.png cart: 160x205 RGBA, one data byte per pixel from the low 2
 * bits of each channel (A<<6 | R<<4 | G<<2 | B). Bytes 0x0000-0x42ff are the
 * ROM, 0x4300-0x7fff the (possibly compressed) code, 0x8000 the version.
 * @param {Uint8Array} bytes
 * @returns {Cart}
 */
export function decodeP8Png(bytes) {
  const { width, height, rgba } = decodePng(bytes);
  if (width !== 160 || height !== 205) {
    throw new Error(`not a PICO-8 cart image: ${width}x${height}, expected 160x205`);
  }
  const data = new Uint8Array(width * height);
  for (let i = 0, p = 0; i < data.length; i++, p += 4) {
    data[i] = ((rgba[p + 3] & 3) << 6) | ((rgba[p] & 3) << 4) | ((rgba[p + 1] & 3) << 2) | (rgba[p + 2] & 3);
  }
  return decodeP8Rom(data);
}

/**
 * Decode a raw 32K cart image (the .p8.rom layout, and what a .p8.png hides).
 * @param {Uint8Array} data at least 0x8000 bytes; byte 0x8000 is the version if present
 * @returns {Cart}
 */
export function decodeP8Rom(data) {
  if (data.length < CODE_END) throw new Error(`cart image is ${data.length} bytes, need 0x8000`);
  const rom = data.slice(0, ROM_SIZE);
  const code = data.subarray(CODE, CODE_END);
  let bytes, codeFormat;
  if (code[0] === 0 && code[1] === 0x70 && code[2] === 0x78 && code[3] === 0x61) {   // "\0pxa"
    bytes = pxaDecompress(code); codeFormat = "pxa";
  } else if (code[0] === 0x3a && code[1] === 0x63 && code[2] === 0x3a && code[3] === 0) {   // ":c:\0"
    bytes = legacyDecompress(code); codeFormat = "legacy";
  } else {
    const end = code.indexOf(0);
    bytes = code.subarray(0, end < 0 ? code.length : end); codeFormat = "raw";
  }
  /** @type {Cart} */
  const cart = { lua: bytesToString(bytes), luaEncoding: "p8scii", rom, codeFormat };
  if (data.length > CODE_END) cart.version = data[CODE_END];
  return cart;
}

/** One JS char per byte (0-255); chunked to stay under argument limits. */
function bytesToString(bytes) {
  let s = "";
  for (let i = 0; i < bytes.length; i += 8192) {
    s += String.fromCharCode.apply(null, bytes.subarray(i, i + 8192));
  }
  return s;
}

const LEGACY_LUT = "\n 0123456789abcdefghijklmnopqrstuvwxyz!#%(){}[]<>+=/*:;.,~_";

/**
 * Pre-0.2.0 ':c:' format: u16be length at 4, then a byte stream where 0x00 =
 * next byte literal, 0x01-0x3b = LUT char, 0x3c-0xff = back-reference with
 * offset (b-0x3c)*16 + (next&15) and length (next>>4)+2.
 */
function legacyDecompress(code) {
  const length = (code[4] << 8) | code[5];
  const out = new Uint8Array(length);
  let n = 0;
  for (let i = 8; i < code.length && n < length; i++) {
    const b = code[i];
    if (b === 0) {
      out[n++] = code[++i];
    } else if (b < 0x3c) {
      out[n++] = LEGACY_LUT.charCodeAt(b - 1);
    } else {
      const next = code[++i];
      const offset = (b - 0x3c) * 16 + (next & 0xf);
      const len = (next >> 4) + 2;
      const from = n - offset;
      // FAKE-08 skips a reference that points before the start
      if (from >= 0) for (let j = 0; j < len && n < length; j++) out[n++] = out[from + j];
    }
  }
  return out.subarray(0, n);
}

/**
 * PICO-8 0.2.0+ 'PXA' format (port of zepto8's pxa_decompress): header
 * "\0pxa", u16be decompressed length, u16be compressed length (incl. the
 * 8-byte header), then an LSB-first bit stream of move-to-front literals and
 * back-references (plus raw-byte runs, encoded as offset 1 with 10-bit form).
 */
function pxaDecompress(code) {
  const length = (code[4] << 8) | code[5];
  const compressed = Math.min((code[6] << 8) | code[7], code.length);
  const limit = compressed * 8;
  let pos = 64;
  const bits = (count) => {
    let n = 0;
    for (let i = 0; i < count && pos < limit; i++, pos++) n |= ((code[pos >> 3] >> (pos & 7)) & 1) << i;
    return n;
  };
  const mtf = new Uint8Array(256);
  for (let i = 0; i < 256; i++) mtf[i] = i;
  const out = new Uint8Array(length);
  let n = 0;
  while (n < length && pos < limit) {
    if (bits(1)) {
      let nbits = 4;
      while (bits(1)) nbits++;
      const idx = bits(nbits) + (1 << nbits) - 16;
      const ch = mtf[idx];
      mtf.copyWithin(1, 0, idx);
      mtf[0] = ch;
      if (!ch) break;
      out[n++] = ch;
    } else {
      const nbits = bits(1) ? (bits(1) ? 5 : 10) : 15;
      const offset = bits(nbits) + 1;
      if (nbits === 10 && offset === 1) {
        for (let ch = bits(8); ch && n < length; ch = bits(8)) out[n++] = ch;
      } else {
        let len = 3, k;
        do { k = bits(3); len += k; } while (k === 7);
        if (offset > n) throw new Error(`PXA code: back-reference ${offset} before start of output at ${n}`);
        for (let i = 0; i < len && n < length; i++, n++) out[n] = out[n - offset];
      }
    }
  }
  return out.subarray(0, n);
}
