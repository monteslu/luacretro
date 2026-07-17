import { test } from "node:test";
import assert from "node:assert";
import { readFileSync, readdirSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const dir = path.join(path.dirname(fileURLToPath(import.meta.url)), "..", "compiler");

// The compiler core must be browser-safe: no node: imports, no Buffer, no
// require() - so the web IDEs can bundle it unchanged (same discipline as the
// SDK compilers).
test("compiler/ is browser-safe (no node:/Buffer/require)", () => {
  for (const f of readdirSync(dir).filter((f) => f.endsWith(".js"))) {
    const src = readFileSync(path.join(dir, f), "utf8");
    // strip comments cheaply for the scan (avoid false hits in prose)
    const code = src.replace(/\/\/[^\n]*/g, "").replace(/\/\*[\s\S]*?\*\//g, "");
    assert.ok(!/from\s+["']node:/.test(code), `${f}: imports a node: module`);
    assert.ok(!/\brequire\s*\(/.test(code), `${f}: uses require()`);
    assert.ok(!/\bBuffer\b/.test(code), `${f}: references Buffer`);
  }
});
