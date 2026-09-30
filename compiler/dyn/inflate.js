// luacretro dyn - DEFLATE (RFC 1951) and zlib (RFC 1950) decompression in
// plain JS, so the cart loader can read .p8.png files in a browser without
// Node's zlib. Handles stored, fixed-Huffman and dynamic-Huffman blocks.

// Length / distance base values and extra-bit counts (RFC 1951 3.2.5).
const LEN_BASE = [3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
  35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258];
const LEN_EXTRA = [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
  3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0];
const DIST_BASE = [1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
  257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577];
const DIST_EXTRA = [0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
  7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13];
// Order the code-length code lengths are transmitted in.
const CL_ORDER = [16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15];

/**
 * Canonical Huffman table: `counts[len]` codes of each length and `symbols`
 * in code order (the "puff" layout - decoded one bit at a time).
 * @typedef {{counts: Uint16Array, symbols: Uint16Array}} Huffman
 */

/**
 * @param {ArrayLike<number>} lengths code length per symbol (0 = unused)
 * @returns {Huffman}
 */
function buildHuffman(lengths) {
  const counts = new Uint16Array(16);
  for (let i = 0; i < lengths.length; i++) counts[lengths[i]]++;
  counts[0] = 0;
  // reject over-subscribed sets (incomplete ones are legal, e.g. one distance code)
  let left = 1;
  for (let len = 1; len < 16; len++) {
    left = (left << 1) - counts[len];
    if (left < 0) throw new Error("inflate: over-subscribed Huffman code lengths");
  }
  const offs = new Uint16Array(16);
  for (let len = 1; len < 15; len++) offs[len + 1] = offs[len] + counts[len];
  const symbols = new Uint16Array(lengths.length);
  for (let i = 0; i < lengths.length; i++) if (lengths[i]) symbols[offs[lengths[i]]++] = i;
  return { counts, symbols };
}

let FIXED_LIT = null, FIXED_DIST = null;
function fixedTables() {
  if (!FIXED_LIT) {
    const l = new Uint8Array(288);
    l.fill(8, 0, 144); l.fill(9, 144, 256); l.fill(7, 256, 280); l.fill(8, 280, 288);
    FIXED_LIT = buildHuffman(l);
    FIXED_DIST = buildHuffman(new Uint8Array(30).fill(5));
  }
  return [FIXED_LIT, FIXED_DIST];
}

/**
 * Inflate a raw DEFLATE stream.
 * @param {Uint8Array} input
 * @param {number} [start=0] byte offset where the stream begins
 * @returns {Uint8Array}
 */
export function inflateRaw(input, start = 0) {
  let pos = start, bitBuf = 0, bitCnt = 0;
  let out = new Uint8Array(Math.max(1024, input.length * 4));
  let outLen = 0;

  const need = (n) => {
    if (outLen + n <= out.length) return;
    let size = out.length * 2;
    while (size < outLen + n) size *= 2;
    const grown = new Uint8Array(size);
    grown.set(out.subarray(0, outLen));
    out = grown;
  };
  const bits = (n) => {
    while (bitCnt < n) {
      if (pos >= input.length) throw new Error("inflate: unexpected end of compressed data");
      bitBuf |= input[pos++] << bitCnt;
      bitCnt += 8;
    }
    const v = bitBuf & ((1 << n) - 1);
    bitBuf >>>= n;
    bitCnt -= n;
    return v;
  };
  /** @param {Huffman} h */
  const decode = (h) => {
    let code = 0, first = 0, index = 0;
    for (let len = 1; len < 16; len++) {
      code |= bits(1);
      const count = h.counts[len];
      if (code - first < count) return h.symbols[index + code - first];
      index += count;
      first = (first + count) << 1;
      code <<= 1;
    }
    throw new Error("inflate: invalid Huffman code");
  };

  let final = 0;
  while (!final) {
    final = bits(1);
    const type = bits(2);
    if (type === 0) {
      // stored: skip to a byte boundary, LEN, NLEN, raw bytes
      bitBuf = 0; bitCnt = 0;
      if (pos + 4 > input.length) throw new Error("inflate: truncated stored block header");
      const len = input[pos] | (input[pos + 1] << 8);
      const nlen = input[pos + 2] | (input[pos + 3] << 8);
      if ((len ^ 0xffff) !== nlen) throw new Error("inflate: stored block length check failed");
      pos += 4;
      if (pos + len > input.length) throw new Error("inflate: truncated stored block");
      need(len);
      out.set(input.subarray(pos, pos + len), outLen);
      outLen += len; pos += len;
      continue;
    }
    let lit, dist;
    if (type === 1) {
      [lit, dist] = fixedTables();
    } else if (type === 2) {
      const hlit = bits(5) + 257, hdist = bits(5) + 1, hclen = bits(4) + 4;
      const clLens = new Uint8Array(19);
      for (let i = 0; i < hclen; i++) clLens[CL_ORDER[i]] = bits(3);
      const cl = buildHuffman(clLens);
      const lens = new Uint8Array(hlit + hdist);
      for (let i = 0; i < hlit + hdist;) {
        const sym = decode(cl);
        if (sym < 16) { lens[i++] = sym; continue; }
        let rep, val = 0;
        if (sym === 16) {
          if (i === 0) throw new Error("inflate: repeat with no previous length");
          val = lens[i - 1]; rep = 3 + bits(2);
        } else if (sym === 17) rep = 3 + bits(3);
        else rep = 11 + bits(7);
        if (i + rep > hlit + hdist) throw new Error("inflate: code lengths overrun");
        while (rep--) lens[i++] = val;
      }
      if (!lens[256]) throw new Error("inflate: missing end-of-block code");
      lit = buildHuffman(lens.subarray(0, hlit));
      dist = buildHuffman(lens.subarray(hlit));
    } else {
      throw new Error("inflate: invalid block type 3");
    }
    for (;;) {
      const sym = decode(lit);
      if (sym < 256) { need(1); out[outLen++] = sym; continue; }
      if (sym === 256) break;
      const li = sym - 257;
      if (li >= 29) throw new Error("inflate: invalid length symbol");
      const len = LEN_BASE[li] + bits(LEN_EXTRA[li]);
      const di = decode(dist);
      if (di >= 30) throw new Error("inflate: invalid distance symbol");
      const d = DIST_BASE[di] + bits(DIST_EXTRA[di]);
      if (d > outLen) throw new Error("inflate: distance reaches before start of output");
      need(len);
      // byte-by-byte: overlapping copies (d < len) are legal and common
      for (let k = 0; k < len; k++, outLen++) out[outLen] = out[outLen - d];
    }
  }
  return out.slice(0, outLen);
}

/**
 * Inflate a zlib-wrapped stream (2-byte header, DEFLATE body, Adler-32).
 * @param {Uint8Array} input
 * @returns {Uint8Array}
 */
export function inflateZlib(input) {
  if (input.length < 2) throw new Error("zlib: stream too short");
  const cmf = input[0], flg = input[1];
  if ((cmf & 0x0f) !== 8) throw new Error(`zlib: unsupported compression method ${cmf & 0x0f}`);
  if (((cmf << 8) | flg) % 31 !== 0) throw new Error("zlib: header check failed");
  if (flg & 0x20) throw new Error("zlib: preset dictionaries are not supported");
  return inflateRaw(input, 2);
}
