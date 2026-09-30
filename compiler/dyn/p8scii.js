// P8SCII <-> Unicode, as FAKE-08 (MIT) converts .p8 text carts on load
// (source/emojiconversion.cpp, itself from zepto8, WTFPL).
//
// A .p8 file stores its Lua source as UTF-8, with every P8SCII glyph
// 0x80-0xff (and the control codes 1-8, 11, 12, 14, 15) written as a
// Unicode character. Loading turns each of those back into its single
// byte; that byte string is what the cart's Lua actually sees.
//
// Conversion rules (FAKE-08's charset::utf8_to_pico8):
//  - ASCII (U+0000-U+007F) passes through as the same byte.
//  - A Unicode sequence listed in CHARSET becomes its byte. The emoji
//    glyphs are two code points, base + U+FE0F (variation selector-16),
//    e.g. "⬅️" = U+2B05 U+FE0F -> 0x8b. Only that exact form matches.
//  - Anything else is NOT mapped: it passes through as its raw UTF-8
//    bytes, each of which then reads as a P8SCII glyph >= 0x80 (so "é"
//    becomes the two bytes 0xc3 0xa9). This includes a bare emoji base
//    without its U+FE0F ("⬅" alone -> 0xe2 0xac 0x85) and a stray
//    U+FE0F. That is FAKE-08's behaviour and deliberately preserved.
//  - A lone UTF-16 surrogate (not valid Unicode) is encoded as U+FFFD.
//
// Browser-safe: no node: imports, no Buffer.

// The full charmap, bytes 0..255, in FAKE-08's order.
const CHARMAP =
  "\0¹²³⁴⁵⁶⁷⁸\t\nᵇᶜ\rᵉᶠ▮■□⁙⁘‖◀▶「」¥•、。゛゜" +
  " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNO" +
  "PQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~○" +
  "█▒🐱⬇️░✽●♥☉웃⌂⬅️😐♪🅾️◆…➡️★⧗⬆️ˇ∧❎▤▥あいうえおか" +
  "きくけこさしすせそたちつてとなにぬねのはひふへほまみむめもやゆよ" +
  "らりるれろわをんっゃゅょアイウエオカキクケコサシスセソタチツテト" +
  "ナニヌネノハヒフヘホマミムメモヤユヨラリルレロワヲンッャュョ◜◝";

/** CHARSET[b] is the Unicode string for P8SCII byte b. */
export const CHARSET = (() => {
  const cps = Array.from(CHARMAP);
  const out = [];
  for (let i = 0; i < cps.length; i++) {
    if (cps[i + 1] === "️") { out.push(cps[i] + "️"); i++; }
    else out.push(cps[i]);
  }
  if (out.length !== 256) throw new Error("p8scii: charmap has " + out.length + " entries");
  return Object.freeze(out);
})();

// Non-ASCII sequences -> byte, keyed by the first code point.
const BY_FIRST = new Map();
CHARSET.forEach((s, b) => {
  if (s.codePointAt(0) < 0x80) return;
  const first = String.fromCodePoint(s.codePointAt(0));
  if (!BY_FIRST.has(first)) BY_FIRST.set(first, []);
  BY_FIRST.get(first).push([s, b]);
});
// longest first, so a base+U+FE0F form wins over any shorter entry
for (const list of BY_FIRST.values()) list.sort((a, b) => b[0].length - a[0].length);

function pushUtf8(out, cp) {
  if (cp < 0x80) out.push(cp);
  else if (cp < 0x800) out.push(0xc0 | (cp >> 6), 0x80 | (cp & 63));
  else if (cp < 0x10000) out.push(0xe0 | (cp >> 12), 0x80 | ((cp >> 6) & 63), 0x80 | (cp & 63));
  else out.push(0xf0 | (cp >> 18), 0x80 | ((cp >> 12) & 63), 0x80 | ((cp >> 6) & 63), 0x80 | (cp & 63));
}

/**
 * Convert a JS (Unicode) string, e.g. .p8 cart source, to P8SCII bytes.
 * @param {string} str
 * @returns {Uint8Array}
 */
export function unicodeToP8scii(str) {
  const out = [];
  let i = 0;
  while (i < str.length) {
    let cp = str.codePointAt(i);
    const w = cp > 0xffff ? 2 : 1;
    if (cp < 0x80) { out.push(cp); i += 1; continue; }
    const list = BY_FIRST.get(String.fromCodePoint(cp));
    let hit = null;
    if (list) for (const e of list) if (str.startsWith(e[0], i)) { hit = e; break; }
    if (hit) { out.push(hit[1]); i += hit[0].length; continue; }
    if (cp >= 0xd800 && cp <= 0xdfff) cp = 0xfffd;       // lone surrogate
    pushUtf8(out, cp);
    i += w;
  }
  return Uint8Array.from(out);
}

/**
 * Convert P8SCII bytes to a Unicode string (diagnostics, printh-style).
 * @param {Uint8Array|number[]} bytes
 * @returns {string}
 */
export function p8sciiToUnicode(bytes) {
  let s = "";
  for (const b of bytes) s += CHARSET[b & 0xff];
  return s;
}
