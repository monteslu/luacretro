// Dynamic tier end-to-end: compile Lua to C, build it with the host C compiler
// against the portable runtime, run it headless and check its output.
import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { execFileSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { buildHost } from "./host/build.mjs";
import { compileLua, toP8scii } from "../compiler/dyn/index.js";

const HERE = path.dirname(fileURLToPath(import.meta.url));

function run(luaFile, ticks = 2) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "lcdyn-"));
  const bin = path.join(dir, "cart");
  const b = buildHost(luaFile, bin);
  assert.ok(b.ok, b.error);
  try {
    return execFileSync(bin, [String(ticks), "-", "/dev/null", String(4 << 20)], { encoding: "utf8" });
  } catch (e) {
    return String(e.stdout ?? "");   /* runtime errors exit non-zero */
  }
}

test("dynamic tier semantics suite", () => {
  const out = run(path.join(HERE, "dyn", "semantics.lua"));
  const bad = out.split("\n").filter((l) => /FAIL|RUNTIME ERROR/.test(l));
  assert.deepEqual(bad, []);
  assert.match(out, /ALL OK/);

});

test("the suite can fail (control)", () => {
  const src = path.join(os.tmpdir(), `lcdyn-control-${process.pid}.lua`);
  fs.writeFileSync(src, 'if 1/3 ~= 0x0.5555 then printh("x FAIL") else printh("x ok") end\nprinth(tostr(2+2))');
  const out = run(src);
  assert.match(out, /x ok/);
  assert.match(out, /^4$/m);
  fs.writeFileSync(src, "local t = nil\nprinth(t.x)");
  const bad = run(src);
  assert.match(bad, /RUNTIME ERROR: .*index a nil value/);
});

test("compile errors are diagnostics, not exceptions", () => {
  const r = compileLua(toP8scii("function f( end"), { file: "bad.lua" });
  assert.equal(r.ok, false);
  assert.ok(r.diagnostics.length > 0);
});
