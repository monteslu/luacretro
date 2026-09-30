import { test } from "node:test";
import assert from "node:assert";
import { readFileSync, existsSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { deflateRawSync, deflateSync } from "node:zlib";

import { loadCart, parseP8Text, decodeP8Png, decodeP8Rom, ROM_SIZE } from "../compiler/dyn/cart.js";
import { inflateRaw, inflateZlib } from "../compiler/dyn/inflate.js";
import { decodePng } from "../compiler/dyn/png.js";

// FAKE-08's own test carts (MIT) are vendored in test/fixtures/fake08.
// Community carts are CC BY-NC-SA: read in place, never copied into this repo.
// Their root is the directory that CONTAINS the luacretro checkout unless
// LUACRETRO_CARTS_ROOT says otherwise; without them (a clean clone, CI) those
// tests are skipped, and setting LUACRETRO_CARTS_ROOT makes them required.
const FAKE08_CARTS = fileURLToPath(new URL("./fixtures/fake08/", import.meta.url));
const ROOT = process.env.LUACRETRO_CARTS_ROOT ?? fileURLToPath(new URL("../../", import.meta.url));
const COMMUNITY = path.join(ROOT, "fake08-build/community-carts");
const HAVE_COMMUNITY = !!process.env.LUACRETRO_CARTS_ROOT || existsSync(COMMUNITY);

/** Read a cart; a missing file FAILS the test (never skips) with the fix. */
function cartBytes(file) {
  assert.ok(existsSync(file),
    `missing test cart ${file} - set LUACRETRO_CARTS_ROOT to the directory holding fake08-build/ (now ${ROOT})`);
  return new Uint8Array(readFileSync(file));
}
const load = (file, opts) => loadCart(cartBytes(file), path.basename(file), opts);

const fnv = (bytes) => {
  let h = 0x811c9dc5;
  for (const b of bytes) { h ^= b; h = Math.imul(h, 0x01000193) >>> 0; }
  return h.toString(16).padStart(8, "0");
};
const strBytes = (s) => Uint8Array.from(s, (c) => c.charCodeAt(0));

// ---------------------------------------------------------------------------
// browser safety (compiler/dyn is not covered by browser-safe.test.js's scan)

test("cart loader files are browser-safe (no node:/Buffer/require)", () => {
  const dir = path.join(path.dirname(fileURLToPath(import.meta.url)), "..", "compiler", "dyn");
  for (const f of ["cart.js", "png.js", "inflate.js"]) {
    const code = readFileSync(path.join(dir, f), "utf8")
      .replace(/\/\/[^\n]*/g, "").replace(/\/\*[\s\S]*?\*\//g, "");
    assert.ok(!/from\s+["']node:/.test(code), `${f}: imports a node: module`);
    assert.ok(!/\brequire\s*\(/.test(code), `${f}: uses require()`);
    assert.ok(!/\bBuffer\b/.test(code), `${f}: references Buffer`);
  }
});

// ---------------------------------------------------------------------------
// inflate

/** BTYPE of the first block of a raw DEFLATE stream. */
const firstBlockType = (raw) => (raw[0] >> 1) & 3;

test("inflate: stored, fixed and dynamic Huffman blocks", () => {
  const text = new TextEncoder().encode("function _update() x+=1 end\n".repeat(400));
  const cases = [
    [deflateRawSync(text, { level: 0 }), 0, text],
    [deflateRawSync(new TextEncoder().encode("a=1"), { level: 9 }), 1, new TextEncoder().encode("a=1")],
    [deflateRawSync(text, { level: 9 }), 2, text],
  ];
  for (const [z, type, want] of cases) {
    assert.strictEqual(firstBlockType(z), type, `zlib produced block type ${firstBlockType(z)}, wanted ${type}`);
    assert.deepStrictEqual(inflateRaw(new Uint8Array(z)), new Uint8Array(want));
  }
  // random bytes: long distances, all literal values, multi-block
  const rnd = new Uint8Array(200000);
  let s = 1;
  for (let i = 0; i < rnd.length; i++) { s = (Math.imul(s, 1103515245) + 12345) >>> 0; rnd[i] = (s >>> 16) & (i % 3 ? 0xff : 0x0f); }
  assert.deepStrictEqual(inflateZlib(new Uint8Array(deflateSync(rnd))), rnd);
  assert.throws(() => inflateRaw(new Uint8Array([0x07])), /invalid block type/);
  assert.throws(() => inflateZlib(new Uint8Array([0x78, 0x00])), /header check/);
});

// ---------------------------------------------------------------------------
// png

function chunk(type, data) {
  const out = new Uint8Array(12 + data.length);
  const v = new DataView(out.buffer);
  v.setUint32(0, data.length);
  for (let i = 0; i < 4; i++) out[4 + i] = type.charCodeAt(i);
  out.set(data, 8);   // CRC left zero: the decoder does not check it
  return out;
}
function makePng(width, height, color, pixels, { interlace = 0, depth = 8 } = {}) {
  const bpp = color === 6 ? 4 : 3, stride = width * bpp;
  const raw = new Uint8Array(height * (stride + 1));
  const paeth = (a, b, c) => {
    const p = a + b - c, pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
  };
  for (let y = 0; y < height; y++) {
    const ft = y % 5;   // every filter type in use
    raw[y * (stride + 1)] = ft;
    for (let x = 0; x < stride; x++) {
      const cur = pixels[y * stride + x];
      const a = x >= bpp ? pixels[y * stride + x - bpp] : 0;
      const b = y ? pixels[(y - 1) * stride + x] : 0;
      const c = x >= bpp && y ? pixels[(y - 1) * stride + x - bpp] : 0;
      const pred = [0, a, b, (a + b) >> 1, paeth(a, b, c)][ft];
      raw[y * (stride + 1) + 1 + x] = (cur - pred) & 0xff;
    }
  }
  const ihdr = new Uint8Array(13);
  const v = new DataView(ihdr.buffer);
  v.setUint32(0, width); v.setUint32(4, height);
  ihdr.set([depth, color, 0, 0, interlace], 8);
  const parts = [new Uint8Array([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk("IHDR", ihdr), chunk("tEXt", new Uint8Array(4)), chunk("IDAT", new Uint8Array(deflateSync(raw))), chunk("IEND", new Uint8Array())];
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let o = 0;
  for (const p of parts) { out.set(p, o); o += p.length; }
  return out;
}

test("png: RGBA and RGB with filter types 0-4; clear errors otherwise", () => {
  const w = 7, h = 11;
  for (const color of [6, 2]) {
    const bpp = color === 6 ? 4 : 3;
    const px = new Uint8Array(w * h * bpp).map((_, i) => (i * 37 + (i >> 3) * 11) & 0xff);
    const img = decodePng(makePng(w, h, color, px));
    assert.strictEqual(img.width, w); assert.strictEqual(img.height, h);
    for (let i = 0; i < w * h; i++) {
      for (let c = 0; c < 3; c++) assert.strictEqual(img.rgba[i * 4 + c], px[i * bpp + c]);
      assert.strictEqual(img.rgba[i * 4 + 3], bpp === 4 ? px[i * 4 + 3] : 255);
    }
  }
  const px = new Uint8Array(4 * 4 * 4);
  assert.throws(() => decodePng(makePng(4, 4, 6, px, { interlace: 1 })), /interlaced/);
  assert.throws(() => decodePng(makePng(4, 4, 6, px, { depth: 16 })), /bit depth 16/);
  assert.throws(() => decodePng(makePng(4, 4, 3, px)), /color type 3/);
  assert.throws(() => decodeP8Png(makePng(4, 4, 6, px)), /4x4, expected 160x205/);
});

// ---------------------------------------------------------------------------
// .p8 text


test("parseP8Text: every section lands at its PICO-8 address", () => {
  const sfx0 = "01" + "1d" + "02" + "03" + "0d530" + "3cf07" + "00000".repeat(30);
  const text = [
    "pico-8 cartridge // http://www.pico-8.com", "version 41",
    "__lua__", "print(\"hi\")", "-- ⬅️ glyph stays unicode", "",
    "__gfx__", "0123456789abcdef" + "0".repeat(112), "f".repeat(128),
    "__label__", "0123456789abcdefghijklmnopqrstuv" + "0".repeat(96),
    "__gff__", "0180" + "0".repeat(252),
    "__map__", "4142" + "0".repeat(252),
    "__sfx__", sfx0, "", "00ff" + "0".repeat(164),
    "__music__", "0b 01424344", "00 05060708", "",
  ].join("\r\n");
  const c = parseP8Text(text);
  assert.strictEqual(c.version, 41);
  assert.strictEqual(c.luaEncoding, "unicode");
  assert.strictEqual(c.lua, "print(\"hi\")\n-- ⬅️ glyph stays unicode\n");
  assert.strictEqual(c.rom.length, ROM_SIZE);
  // gfx: "01" -> 0x10 (left pixel in the low nibble)
  assert.deepStrictEqual([...c.rom.subarray(0, 8)], [0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe]);
  assert.strictEqual(c.rom[64], 0xff);
  assert.deepStrictEqual([...c.rom.subarray(0x3000, 0x3002)], [0x01, 0x80]);
  assert.deepStrictEqual([...c.rom.subarray(0x2000, 0x2002)], [0x41, 0x42]);
  // sfx 0: note 0 = pitch 0x0d wave 5 vol 3 fx 0; note 1 = pitch 0x3c wave 15 (custom 7) vol 0 fx 7
  assert.deepStrictEqual([...c.rom.subarray(0x3200, 0x3204)], [0x0d | (1 << 6), 1 | (3 << 1), 0x3c | (3 << 6), 1 | (7 << 4) | 0x80]);
  assert.deepStrictEqual([...c.rom.subarray(0x3240, 0x3244)], [0x01, 0x1d, 0x02, 0x03]);
  // the blank sfx line is skipped: sfx 1 is the "00ff" line, sfx 2+ keep speed 16
  assert.strictEqual(c.rom[0x3200 + 68 + 65], 0xff);
  assert.strictEqual(c.rom[0x3200 + 2 * 68 + 65], 16);
  // music flags ride in bit 7: 0x0b = loop-begin | loop-end | mode
  assert.deepStrictEqual([...c.rom.subarray(0x3100, 0x3108)], [0x81, 0xc2, 0x43, 0xc4, 5, 6, 7, 8]);
  // patterns the text omits are PICO-8's empty pattern (all channels off)
  assert.deepStrictEqual([...c.rom.subarray(0x3108, 0x310c)], [0x41, 0x42, 0x43, 0x44]);
  assert.deepStrictEqual([...c.rom.subarray(0x31fc, 0x3200)], [0x41, 0x42, 0x43, 0x44]);
  assert.deepStrictEqual([...c.label.subarray(0, 32)], Array.from({ length: 32 }, (_, i) => i));
});

test("parseP8Text: #include via resolveInclude (file, cart, cart tab) and a clear error", () => {
  const files = {
    "lib.lua": "function f() return 1 end\n",
    "other.p8": "pico-8 cartridge\nversion 41\n__lua__\ntab0()\n-->8\ntab1()\n__gfx__\n",
  };
  const text = "pico-8 cartridge\nversion 41\n__lua__\n#include lib.lua\n#include other.p8:1\n#include other.p8\nmain()\n";
  const c = parseP8Text(text, { name: "main.p8", resolveInclude: (p) => files[p] ?? null });
  assert.strictEqual(c.lua, "function f() return 1 end\ntab1()\ntab0()\n-->8\ntab1()\nmain()");
  assert.deepStrictEqual(c.includes, [
    { path: "lib.lua", line: 4 }, { path: "other.p8", line: 5, tab: 1 }, { path: "other.p8", line: 6 }]);
  assert.throws(() => parseP8Text(text, { name: "main.p8" }), /main\.p8:4: cannot resolve "#include lib\.lua"/);
});

test("FAKE-08 includetest.p8 resolves its include from the cart directory", () => {
  const c = load(path.join(FAKE08_CARTS, "includetest.p8"), {
    resolveInclude: (p) => readFileSync(path.join(FAKE08_CARTS, p), "utf8"),
  });
  assert.match(c.lua, /print\("included successfully!"\)/);
  assert.deepStrictEqual(c.includes, [{ path: "testinclude.blah", line: 4 }]);
});

// ---------------------------------------------------------------------------
// FAKE-08's own test carts (MIT)

// cartparsetest.p8 carries two records its .p8.png does not: sfx 2 and music
// pattern 1 were hand-added to the text (FAKE-08's carttests.cpp checks
// song 1 = 155,125,239,197 on the .p8 only; the PICO-8-saved .png has the
// empty pattern 41 42 43 44 and an empty sfx 2 there). Everything else must be
// byte-identical, and those two records must be exactly as described.
const SONG1 = [0x3104, 0x3108], SFX2 = [0x3200 + 2 * 68, 0x3200 + 3 * 68];
const inRange = (i, [lo, hi]) => i >= lo && i < hi;

test("cartparsetest.p8 and cartparsetest.p8.png give identical ROM and Lua", () => {
  const t = load(path.join(FAKE08_CARTS, "cartparsetest.p8"));
  const p = load(path.join(FAKE08_CARTS, "cartparsetest.p8.png"));
  assert.strictEqual(t.luaEncoding, "unicode");
  assert.strictEqual(p.luaEncoding, "p8scii");
  assert.strictEqual(p.codeFormat, "raw");
  assert.strictEqual(t.lua, "a=1");
  assert.strictEqual(p.lua, t.lua);
  assert.strictEqual(p.version, 29);
  assert.strictEqual(t.version, 29);
  const diffs = [];
  for (let i = 0; i < ROM_SIZE; i++) {
    if (p.rom[i] !== t.rom[i] && !inRange(i, SONG1) && !inRange(i, SFX2)) diffs.push(i.toString(16));
  }
  assert.deepStrictEqual(diffs, [], "ROM bytes differ between .p8 and .p8.png");
  assert.deepStrictEqual([...p.rom.subarray(...SONG1)], [0x41, 0x42, 0x43, 0x44]);
  const emptySfx = new Array(68).fill(0);
  emptySfx[65] = 16;
  assert.deepStrictEqual([...p.rom.subarray(...SFX2)], emptySfx);
  assert.deepStrictEqual([...t.rom.subarray(0x3288, 0x328c)], [139, 52, 216, 78]);   // "0b223 18374" hand-added
  // the same values FAKE-08's carttests.cpp checks (text cart has the extras)
  assert.deepStrictEqual([...t.rom.subarray(...SONG1)], [155, 125, 239, 197]);
  const r = p.rom;
  assert.deepStrictEqual([...r.subarray(0, 4)], [255, 1, 2, 3]);
  assert.deepStrictEqual([...r.subarray(0x3000, 0x3002)], [0, 1]);
  assert.deepStrictEqual([...r.subarray(0x2000, 0x2002)], [1, 0]);
  assert.strictEqual(r[0x3200], 13);          // sfx 0 note 0 byte 0
  assert.strictEqual(r[0x3201], 6);           // sfx 0 note 0 byte 1 (volume 3)
  assert.strictEqual(r[0x3200 + 65], 29);     // sfx 0 speed
  assert.deepStrictEqual([...r.subarray(0x3100, 0x3104)], [1, 66, 67, 68]);
});

test("test_legacypng_cart.p8.png decodes (old-format cart)", () => {
  const c = load(path.join(FAKE08_CARTS, "test_legacypng_cart.p8.png"));
  assert.strictEqual(c.lua, "print(\"0.1.10c\")\n");
  assert.strictEqual(c.luaEncoding, "p8scii");
  assert.deepStrictEqual([...c.rom.subarray(0, 4)], [0, 0, 0, 0]);
});

// ---------------------------------------------------------------------------
// community carts (CC BY-NC-SA: read in place, never committed)
//
// ROM/Lua hashes were locked after the FAKE-08 oracle cross-check matched the
// ROM byte-for-byte (internal-luacretro/scratch/cart/oracle.mjs).

const COMMUNITY_CARTS = {
  "cart_15133.p8.png": { format: "legacy", version: 5, first: "-- ~celeste~", len: 26589, rom: "b9d2bc4c", lua: "987e2161" },
  "combopool.p8.png": { format: "pxa", version: 41, first: "--combo pool", len: 20230, rom: "88923bf5", lua: "f5c3b2b0" },
  "jelpi.p8.png": { format: "legacy", version: 18, first: "-- the adventures of jelpi", len: 10911, rom: "c79db82c", lua: "6c3c4199" },
  // these three downloads are broken on disk: two are 0 bytes, one is an HTTP 404 page
  "cart_28577.p8.png": { error: /cart file is empty/ },
  "cp_34bf02.png": { error: /cart file is empty/ },
  "tweetjam3_3.p8.png": { error: /not a PICO-8 cart .*<!DOCTYPE HTML/ },
};

for (const [file, want] of Object.entries(COMMUNITY_CARTS)) {
  test(`community cart ${file}`, { skip: !HAVE_COMMUNITY && "community carts not present (set LUACRETRO_CARTS_ROOT)" }, () => {
    const bytes = cartBytes(path.join(COMMUNITY, file));
    if (want.error) {
      assert.throws(() => loadCart(bytes, file), want.error);
      return;
    }
    const c = loadCart(bytes, file);
    assert.strictEqual(c.codeFormat, want.format);
    assert.strictEqual(c.luaEncoding, "p8scii");
    assert.strictEqual(c.version, want.version);
    assert.strictEqual(c.lua.split("\n")[0], want.first);
    assert.strictEqual(c.lua.length, want.len);
    assert.ok(![...c.lua].some((ch) => ch.charCodeAt(0) > 255), "p8scii Lua is one byte per char");
    assert.strictEqual(fnv(c.rom), want.rom, "ROM hash");
    assert.strictEqual(fnv(strBytes(c.lua)), want.lua, "Lua hash");
  });
}

test("decodeP8Rom takes the raw 32K image a .p8.png hides", () => {
  const data = new Uint8Array(0x8001);
  data[5] = 0x42;
  data.set(new TextEncoder().encode("x=2"), 0x4300);
  data[0x8000] = 41;
  const c = decodeP8Rom(data);
  assert.strictEqual(c.lua, "x=2");
  assert.strictEqual(c.version, 41);
  assert.strictEqual(c.rom[5], 0x42);
});

test("loadCart rejects unknown input clearly", () => {
  assert.throws(() => loadCart(new Uint8Array(), "x.p8"), /x\.p8: cart file is empty/);
  assert.throws(() => loadCart(new TextEncoder().encode("hello"), "x.p8"), /x\.p8: not a PICO-8 cart/);
});
