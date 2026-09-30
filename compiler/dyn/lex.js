// luacretro dynamic tier - lexer for the full PICO-8 Lua dialect.
//
// Input is P8SCII: a JS string whose char codes are bytes 0-255 (a .p8 text
// cart is converted from unicode first). Tokens follow z8lua's llex.c, the
// lexer PICO-8 players use: PICO-8 operators (!=, \, ^^, <<>, >><, >>>,
// compound assignments, @ % $ peeks, ? print), // comments, binary and hex
// literals with fractions, PICO-8's string escapes (\* \# \- \| \+ \^ map
// to control bytes 1-6), and identifiers that may contain P8SCII glyph
// bytes (0x10-0x1f and 0x7f-0xff).

const KEYWORDS = new Set([
  "and", "break", "do", "else", "elseif", "end", "false", "for", "function",
  "goto", "if", "in", "local", "nil", "not", "or", "repeat", "return", "then",
  "true", "until", "while",
]);

/** z8lua lisspecial: glyph bytes that may appear in names */
function isSpecial(c) { return (c >= 0x10 && c < 0x20) || c >= 0x7f; }
function isAlpha(c) {
  return (c >= 65 && c <= 90) || (c >= 97 && c <= 122) || c === 95 || isSpecial(c);
}
function isDigit(c) { return c >= 48 && c <= 57; }
function isHex(c) { return isDigit(c) || (c >= 65 && c <= 70) || (c >= 97 && c <= 102); }
function isAlnum(c) { return isAlpha(c) || isDigit(c); }

/**
 * Convert a numeral's text to 16.16 bits exactly like z8lua's luaO_str2d:
 * decimal goes through strtod then fix32(double) = int32(int64(d*65536));
 * hex/binary build the bits directly (integer part wraps at 16 bits).
 * @param {string} text
 * @returns {number|null} signed 32-bit bits, or null if malformed
 */
export function numeralBits(text) {
  if (/[nN]/.test(text)) return null;
  const any = (base) => {
    let s = text, i = 2, r = 0, f = 0, ni = 0, nf = 0;
    const dig = (ch) => {
      const d = parseInt(ch, 16);
      return Number.isNaN(d) || d >= base ? -1 : d;
    };
    for (; i < s.length; i++) {
      const d = dig(s[i]);
      if (d < 0) break;
      r = (r * base + d) & 0xffff;
      ni++;
    }
    if (s[i] === ".") {
      i++;
      let max = base === 2 ? 16 : 4;
      for (; i < s.length; i++) {
        const d = dig(s[i]);
        if (d < 0) break;
        if (max > 0) { f = f * base + d; nf++; }
        max--;
      }
    }
    if (ni === 0 && nf === 0) return null;
    if (i !== s.length) return null;
    const shift = base === 2 ? nf : nf * 4;
    const fracBits = nf ? ((f << 16) >>> shift) & 0xffff : 0;
    return ((r << 16) | fracBits) | 0;
  };
  if (/[xX]/.test(text)) return /^0[xX]/.test(text) ? any(16) : null;
  if (/^0[bB]/.test(text)) return any(2);
  if (!/^[0-9]*\.?[0-9]*$/.test(text) || text === "." || text === "") return null;
  const d = parseFloat(text);
  if (!Number.isFinite(d)) return null;
  const scaled = Math.trunc(d * 65536);
  // int64 -> int32 truncation (two's complement wrap)
  return Number(BigInt.asIntN(32, BigInt(scaled)));
}

/**
 * @typedef {{t:string, v?:any, line:number, col:number}} Token
 * @param {string} src P8SCII text (char codes 0-255)
 * @param {string} file
 * @returns {{tokens: Token[], diagnostics: object[]}}
 */
export function lexFull(src, file) {
  const tokens = [];
  const diagnostics = [];
  let i = 0, line = 1, col = 1;
  const n = src.length;
  const cc = (k) => (k < n ? src.charCodeAt(k) : -1);
  const err = (msg, l = line, c = col) =>
    diagnostics.push({ file, line: l, col: c, severity: "error", message: msg });
  const adv = (k = 1) => {
    while (k-- > 0 && i < n) {
      if (src[i] === "\n") { line++; col = 1; } else col++;
      i++;
    }
  };
  const push = (t, v, l, c) => tokens.push({ t, v, line: l, col: c });

  // long bracket level at i (i at '['): returns level or -1
  const longLevel = (at) => {
    if (src[at] !== "[") return -1;
    let k = at + 1, lvl = 0;
    while (src[k] === "=") { k++; lvl++; }
    return src[k] === "[" ? lvl : -1;
  };
  const readLong = (lvl, l, c) => {
    adv(lvl + 2);
    if (src[i] === "\r") adv();
    if (src[i] === "\n") adv();
    const close = "]" + "=".repeat(lvl) + "]";
    const end = src.indexOf(close, i);
    if (end < 0) { err("unfinished long string/comment", l, c); const s = src.slice(i); adv(n - i); return s; }
    const s = src.slice(i, end);
    adv(end - i + close.length);
    return s;
  };

  while (i < n) {
    const c = cc(i);
    const l = line, co = col;
    if (c === 10 || c === 13 || c === 32 || c === 9 || c === 11 || c === 12) { adv(); continue; }
    if (c === 45 /* - */ && cc(i + 1) === 45) {
      adv(2);
      const lvl = longLevel(i);
      if (lvl >= 0) { readLong(lvl, l, co); continue; }
      while (i < n && src[i] !== "\n") adv();
      continue;
    }
    if (c === 47 /* / */ && cc(i + 1) === 47) {
      while (i < n && src[i] !== "\n") adv();
      continue;
    }
    if (isDigit(c) || (c === 46 && isDigit(cc(i + 1)))) {
      let s = src[i];
      adv();
      let hexa = false;
      if (s === "0" && (src[i] === "x" || src[i] === "X")) { s += src[i]; adv(); hexa = true; }
      else if (s === "0" && (src[i] === "b" || src[i] === "B")) { s += src[i]; adv(); }
      while (i < n && ((hexa ? isHex(cc(i)) : isDigit(cc(i))) || src[i] === ".")) {
        // stop before '..' (concat) following a number like 1..2
        if (src[i] === "." && src[i + 1] === "." ) break;
        s += src[i];
        adv();
      }
      const bits = numeralBits(s);
      if (bits === null) err(`malformed number near '${s}'`, l, co);
      push("number", bits ?? 0, l, co);
      continue;
    }
    if (isAlpha(c)) {
      let s = "";
      while (i < n && isAlnum(cc(i))) { s += src[i]; adv(); }
      if (KEYWORDS.has(s)) push(s, s, l, co);
      else push("name", s, l, co);
      continue;
    }
    if (c === 34 || c === 39) {
      const q = src[i];
      adv();
      let out = "";
      let ok = false;
      while (i < n) {
        const ch = src[i];
        if (ch === q) { adv(); ok = true; break; }
        if (ch === "\n" || ch === "\r") break;
        if (ch !== "\\") { out += ch; adv(); continue; }
        adv();
        const e = src[i];
        const map = { "*": 1, "#": 2, "-": 3, "|": 4, "+": 5, "^": 6, a: 7, b: 8, f: 12, n: 10, r: 13, t: 9, v: 11 };
        if (e in map) { out += String.fromCharCode(map[e]); adv(); continue; }
        if (e === "\\" || e === '"' || e === "'") { out += e; adv(); continue; }
        if (e === "\n" || e === "\r") { out += "\n"; adv(); continue; }
        if (e === "x") {
          adv();
          const h = src.slice(i, i + 2);
          if (!/^[0-9a-fA-F]{2}$/.test(h)) { err("hexadecimal digit expected", line, col); continue; }
          out += String.fromCharCode(parseInt(h, 16));
          adv(2);
          continue;
        }
        if (e === "z") {
          adv();
          while (i < n && /[ \t\r\n\v\f]/.test(src[i])) adv();
          continue;
        }
        if (e !== undefined && isDigit(e.charCodeAt(0))) {
          let d = "";
          while (d.length < 3 && i < n && isDigit(cc(i))) { d += src[i]; adv(); }
          const v = parseInt(d, 10);
          if (v > 255) err("decimal escape too large", line, col);
          out += String.fromCharCode(v & 255);
          continue;
        }
        err("invalid escape sequence", line, col);
        adv();
      }
      if (!ok) err("unfinished string", l, co);
      push("string", out, l, co);
      continue;
    }
    if (c === 91 /* [ */) {
      const lvl = longLevel(i);
      if (lvl >= 0) { push("string", readLong(lvl, l, co), l, co); continue; }
      if (src[i + 1] === "=") { err("invalid long string delimiter", l, co); }
      push("[", "[", l, co);
      adv();
      continue;
    }
    // operators, longest match first
    const ops4 = [">>>=", "<<>=", ">><="];
    const ops3 = ["...", "..=", ">>>", "<<>", ">><", "<<=", ">>=", "^^="];
    const ops2 = ["==", "~=", "!=", "<=", ">=", "..", "::", "<<", ">>", "^^",
      "+=", "-=", "*=", "/=", "%=", "^=", "\\=", "&=", "|="];
    let matched = null;
    for (const o of ops4) if (src.startsWith(o, i)) { matched = o; break; }
    if (!matched) for (const o of ops3) if (src.startsWith(o, i)) { matched = o; break; }
    if (!matched) for (const o of ops2) if (src.startsWith(o, i)) { matched = o; break; }
    if (matched) {
      const t = matched === "!=" ? "~=" : matched;
      push(t, matched, l, co);
      adv(matched.length);
      continue;
    }
    const ch = src[i];
    if ("+-*/%^#&|~<>=(){}[];:,.\\@$?".includes(ch)) {
      push(ch, ch, l, co);
      adv();
      continue;
    }
    err(`unexpected symbol near '${ch}' (0x${c.toString(16)})`, l, co);
    adv();
  }
  tokens.push({ t: "eof", v: "", line, col });
  return { tokens, diagnostics };
}
