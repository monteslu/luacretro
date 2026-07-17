// luacretro lexer - PICO-8-flavored Lua tokens (shared front-end for the
// gtlua / gbalua / mdlua console SDKs).
//
// Dialect notes (see PICO8.md):
//  - `//` starts a comment (PICO-8/C style); `\` is floor division
//  - `!=` is an alias for `~=`
//  - numbers are 16.16 fixed point: decimal/hex/binary literals may carry
//    fractions; every number token carries `fixed` (the 32-bit 16.16 bits)
//    and `isInt` (true when the literal is integral and fits 16 bits)
//  - PICO-8 button glyphs (⬅️➡️⬆️⬇️🅾️❎) lex as number tokens 0..5

const KEYWORDS = new Set([
  "and", "break", "do", "else", "elseif", "end", "false", "for", "function",
  "if", "in", "local", "nil", "not", "or", "repeat", "return", "then",
  "true", "until", "while", "goto",
]);

// PICO-8 button glyphs -> btn()/btnp() indices. The emoji include optional
// variation selectors (U+FE0F); match longest-first.
// P8SCII: the raw single-byte control forms for the six buttons as they sit in
// a .p8/.p8.png cart before any UTF-8 rendering (left=8b right=91 up=94 down=83
// O=8e X=97) - accept those too so imported carts lex without a pre-pass.
const P8SCII = [[0x8b, 0], [0x91, 1], [0x94, 2], [0x83, 3], [0x8e, 4], [0x97, 5]]
  .map(([code, v]) => [String.fromCharCode(code), v]);
const GLYPHS = [
  ["⬅️", 0], ["⬅", 0], ["➡️", 1], ["➡", 1],
  ["⬆️", 2], ["⬆", 2], ["⬇️", 3], ["⬇", 3],
  ["🅾️", 4], ["🅾", 4], ["❎", 5], ["❌", 5],
  ...P8SCII,
];

/** Convert a JS number (value) to 16.16 bits, wrapped to signed 32-bit. */
export function toFixed(value) {
  return (Math.round(value * 65536) | 0);
}

/**
 * @typedef {{type:string, value:string|number, fixed?:number, isInt?:boolean, line:number, col:number}} Token
 */

/**
 * @param {string} src
 * @param {string} file
 * @returns {{tokens: Token[], diagnostics: object[]}}
 */
export function lex(src, file) {
  const tokens = [];
  const diagnostics = [];
  let i = 0, line = 1, col = 1;

  const err = (msg, l = line, c = col) =>
    diagnostics.push({ file, line: l, col: c, severity: "error", message: msg });

  const isDigit = (ch) => ch >= "0" && ch <= "9";
  const isHex = (ch) => isDigit(ch) || (ch >= "a" && ch <= "f") || (ch >= "A" && ch <= "F");
  const isBin = (ch) => ch === "0" || ch === "1";
  const isNameStart = (ch) => /[A-Za-z_]/.test(ch);
  const isName = (ch) => /[A-Za-z0-9_]/.test(ch);

  function advance(n = 1) {
    while (n-- > 0) {
      if (src[i] === "\n") { line++; col = 1; } else { col++; }
      i++;
    }
  }

  function pushNumber(value, isIntLiteral, l, c) {
    const intVal = Math.trunc(value);
    const isInt = isIntLiteral && intVal >= -32768 && intVal <= 32767;
    if (value > 32767.9999847 || value < -32768) {
      err(`number ${value} is outside the 16.16 range (-32768 .. 32767.99998)`, l, c);
    }
    tokens.push({ type: "number", value, fixed: toFixed(value), isInt, line: l, col: c });
  }

  // Hex/binary literals are BIT PATTERNS for the 16.16 word, not decimal values.
  // PICO-8 reads `0xffff` as the raw fixed-point bits (which wraps to -1.0 as a
  // signed 16.16), and `0xf0f0.f0f0` / `0b1010...` as direct masks - all common
  // in band()/fillp()/poke() code. So build the 16.16 value from the bits and
  // let it wrap (two's complement), instead of range-checking a huge decimal.
  function pushBitsNumber(intPart, fracPart, radix, l, c) {
    const digits = radix === 16 ? 4 : 1;   // bits per hex/bin fractional digit
    let bits = (parseInt(intPart || "0", radix) & 0xffff) << 16;
    if (fracPart) {
      // fractional digits fill the low 16 bits from the top down
      let frac = 0, shift = 16;
      for (const d of fracPart) { shift -= digits; if (shift < 0) break; frac |= parseInt(d, radix) << shift; }
      bits |= frac & 0xffff;
    }
    bits |= 0;   // wrap to signed 32-bit
    const value = bits / 65536;
    // integral when the low 16 bits are clear; carries the wrapped signed value
    const isInt = !fracPart && Number.isInteger(value) && value >= -32768 && value <= 32767;
    tokens.push({ type: "number", value, fixed: bits, isInt, line: l, col: c });
  }

  while (i < src.length) {
    const ch = src[i];

    if (ch === " " || ch === "\t" || ch === "\r" || ch === "\n") { advance(); continue; }

    // comments: -- line, --[[ block ]], // line (PICO-8/C style)
    if (ch === "-" && src[i + 1] === "-") {
      if (src[i + 2] === "[" && src[i + 3] === "[") {
        const end = src.indexOf("]]", i + 4);
        if (end === -1) { err("unterminated block comment"); i = src.length; break; }
        advance(end + 2 - i);
      } else {
        while (i < src.length && src[i] !== "\n") advance();
      }
      continue;
    }
    if (ch === "/" && src[i + 1] === "/") {
      while (i < src.length && src[i] !== "\n") advance();
      continue;
    }

    const startLine = line, startCol = col;

    // button glyphs
    let matchedGlyph = false;
    for (const [g, v] of GLYPHS) {
      if (src.startsWith(g, i)) {
        advance(g.length);
        pushNumber(v, true, startLine, startCol);
        matchedGlyph = true;
        break;
      }
    }
    if (matchedGlyph) continue;

    if (isDigit(ch) || (ch === "." && isDigit(src[i + 1]))) {
      if (ch === "0" && (src[i + 1] === "x" || src[i + 1] === "X")) {
        advance(2);
        let intPart = "", fracPart = "";
        while (i < src.length && isHex(src[i])) { intPart += src[i]; advance(); }
        if (src[i] === "." ) {
          advance();
          while (i < src.length && isHex(src[i])) { fracPart += src[i]; advance(); }
        }
        if (intPart === "" && fracPart === "") err("malformed hex literal", startLine, startCol);
        pushBitsNumber(intPart, fracPart, 16, startLine, startCol);
        continue;
      }
      if (ch === "0" && (src[i + 1] === "b" || src[i + 1] === "B")) {
        advance(2);
        let intPart = "", fracPart = "";
        while (i < src.length && isBin(src[i])) { intPart += src[i]; advance(); }
        if (src[i] === ".") {
          advance();
          while (i < src.length && isBin(src[i])) { fracPart += src[i]; advance(); }
        }
        if (intPart === "" && fracPart === "") err("malformed binary literal", startLine, startCol);
        pushBitsNumber(intPart, fracPart, 2, startLine, startCol);
        continue;
      }
      let intPart = "", fracPart = "", sawDot = false;
      while (i < src.length && isDigit(src[i])) { intPart += src[i]; advance(); }
      if (src[i] === "." && isDigit(src[i + 1] ?? "")) {
        sawDot = true;
        advance();
        while (i < src.length && isDigit(src[i])) { fracPart += src[i]; advance(); }
      } else if (src[i] === "." && src[i + 1] !== ".") {
        // trailing dot: "1." - treat as integral
        advance();
      }
      const value = parseFloat(`${intPart || "0"}.${fracPart || "0"}`);
      pushNumber(value, !sawDot || fracPart === "", startLine, startCol);
      continue;
    }

    if (isNameStart(ch)) {
      let text = "";
      while (i < src.length && isName(src[i])) { text += src[i]; advance(); }
      tokens.push({
        type: KEYWORDS.has(text) ? text : "name",
        value: text, line: startLine, col: startCol,
      });
      continue;
    }

    if (ch === '"' || ch === "'") {
      const quote = ch;
      advance();
      let text = "";
      // Backslash escapes: a \<char> is TWO source chars that must NOT terminate
      // the string (a bare \" or \\ would otherwise close it early). We keep the
      // escape sequence verbatim in the token - emit re-escapes for C output and
      // P8SCII/glyph passes read the raw \^ codes - so \" \\ \n etc. pass through.
      while (i < src.length && src[i] !== quote && src[i] !== "\n") {
        if (src[i] === "\\" && i + 1 < src.length) {
          text += src[i]; advance();
          text += src[i]; advance();     // consume the escaped char, whatever it is
          continue;
        }
        text += src[i]; advance();
      }
      if (src[i] !== quote) err("unterminated string");
      else advance();
      tokens.push({ type: "string", value: text, line: startLine, col: startCol });
      continue;
    }

    // long string: [[ ... ]] (and the [=[ ... ]=] level form). Spans newlines,
    // no escape processing - PICO-8 carts use these for level grids and credits.
    if (ch === "[" && (src[i + 1] === "[" || src[i + 1] === "=")) {
      let eq = 0;
      while (src[i + 1 + eq] === "=") eq++;
      if (src[i + 1 + eq] === "[") {
        const open = `[${"=".repeat(eq)}[`;
        const close = `]${"=".repeat(eq)}]`;
        advance(open.length);
        // Lua drops a leading newline immediately after the opening bracket
        if (src[i] === "\r") advance();
        if (src[i] === "\n") advance();
        let text = "";
        const end = src.indexOf(close, i);
        if (end === -1) { err("unterminated long string", startLine, startCol); i = src.length; }
        else { text = src.slice(i, end); advance(end - i); advance(close.length); }
        tokens.push({ type: "string", value: text, line: startLine, col: startCol });
        continue;
      }
    }

    // operators, longest first
    const push = (type, len) => {
      tokens.push({ type, value: type, line: startLine, col: startCol });
      advance(len);
    };
    const three = src.slice(i, i + 3);
    const two = src.slice(i, i + 2);
    if (three === "..=" ) { push("..=", 3); continue; }
    if (three === ">>>" || three === "<<>" || three === "><<") { push(three, 3); continue; }
    if (two === "!=") { tokens.push({ type: "~=", value: "!=", line: startLine, col: startCol }); advance(2); continue; }
    if (["==", "~=", "<=", ">=", "..", "+=", "-=", "*=", "/=", "%=", "^=",
         "<<", ">>", "^^", "\\="].includes(two)) { push(two, 2); continue; }
    if ("+-*/%^#<>=(){}[];:,.\\&|~?@$".includes(ch)) { push(ch, 1); continue; }

    err(`unexpected character '${ch}'`);
    advance();
  }

  tokens.push({ type: "eof", value: "", line, col });
  return { tokens, diagnostics };
}
