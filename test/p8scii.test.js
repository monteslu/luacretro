import { test } from "node:test";
import assert from "node:assert";
import { unicodeToP8scii, p8sciiToUnicode, CHARSET } from "../compiler/dyn/p8scii.js";

const bytes = (...b) => Uint8Array.from(b);

test("charmap has 256 distinct entries", () => {
  assert.strictEqual(CHARSET.length, 256);
  assert.strictEqual(new Set(CHARSET).size, 256);
});

test("every byte round-trips through Unicode", () => {
  for (let b = 0; b < 256; b++) {
    const u = p8sciiToUnicode([b]);
    assert.deepStrictEqual(unicodeToP8scii(u), bytes(b), `byte ${b} (${JSON.stringify(u)})`);
  }
  const all = Uint8Array.from({ length: 256 }, (_, i) => i);
  assert.deepStrictEqual(unicodeToP8scii(p8sciiToUnicode(all)), all);
});

test("ASCII passes through unchanged", () => {
  const s = "print(\"hi\\n\")\t-- 0x7e ~";
  assert.deepStrictEqual(unicodeToP8scii(s), Uint8Array.from([...s].map((c) => c.charCodeAt(0))));
  assert.strictEqual(p8sciiToUnicode(unicodeToP8scii(s)), s);
});

test("emoji glyphs with variation selector-16", () => {
  const cases = { "⬇️": 0x83, "⬅️": 0x8b, "🅾️": 0x8e, "➡️": 0x91, "⬆️": 0x94 };
  for (const [u, b] of Object.entries(cases)) {
    assert.strictEqual([...u].length, 2, `${u} is base + U+FE0F`);
    assert.deepStrictEqual(unicodeToP8scii(u), bytes(b), u);
    assert.strictEqual(p8sciiToUnicode([b]), u);
  }
  assert.deepStrictEqual(unicodeToP8scii("⬅️➡️⬆️⬇️🅾️❎"), bytes(0x8b, 0x91, 0x94, 0x83, 0x8e, 0x97));
  assert.deepStrictEqual(unicodeToP8scii("btn(⬅️)"), bytes(98, 116, 110, 40, 0x8b, 41));
});

test("single-code-point glyphs and control-code forms", () => {
  assert.deepStrictEqual(unicodeToP8scii("█▒🐱"), bytes(0x80, 0x81, 0x82));
  assert.deepStrictEqual(unicodeToP8scii("❎"), bytes(0x97));
  assert.deepStrictEqual(unicodeToP8scii("◜◝"), bytes(0xfe, 0xff));
  assert.deepStrictEqual(unicodeToP8scii("○"), bytes(0x7f));
  assert.deepStrictEqual(unicodeToP8scii("▮■□"), bytes(0x10, 0x11, 0x12));
  // ¹..⁸ are bytes 1..8, ᵇ ᶜ ᵉ ᶠ are 11 12 14 15 (P8SCII control codes)
  assert.deepStrictEqual(unicodeToP8scii("¹²³⁴⁵⁶⁷⁸ᵇᶜᵉᶠ"), bytes(1, 2, 3, 4, 5, 6, 7, 8, 11, 12, 14, 15));
});

test("unmapped characters pass through as raw UTF-8 bytes (FAKE-08)", () => {
  assert.deepStrictEqual(unicodeToP8scii("é"), bytes(0xc3, 0xa9));
  // bare emoji base without U+FE0F is not a glyph
  assert.deepStrictEqual(unicodeToP8scii("⬅"), bytes(0xe2, 0xac, 0x85));
  // stray variation selector
  assert.deepStrictEqual(unicodeToP8scii("a️"), bytes(0x61, 0xef, 0xb8, 0x8f));
  // astral, unmapped
  assert.deepStrictEqual(unicodeToP8scii("😀"), bytes(0xf0, 0x9f, 0x98, 0x80));
  // lone surrogate -> U+FFFD
  assert.deepStrictEqual(unicodeToP8scii("\uD800"), bytes(0xef, 0xbf, 0xbd));
});

test("no mapped sequence is a prefix of another", () => {
  for (const a of CHARSET) for (const b of CHARSET)
    if (a !== b && a.length > 1 && b.startsWith(a)) assert.fail(`${a} prefixes ${b}`);
});
