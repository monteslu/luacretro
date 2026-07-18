import { test } from "node:test";
import assert from "node:assert";
import { compile } from "../compiler/index.js";

// Minimal per-target builtin tables (enough to exercise the shared paths
// without vendoring the whole SDK tables). Real byte-identity is gated in each
// SDK's own suite via test/golden-c; this proves the front-end compiles for
// every target and the target seams fire correctly.
const CORE = {
  cls:      { params: [["color", true]], ret: "void", c: "gt_p8_cls" },
  rectfill: { params: [["coord", false], ["coord", false], ["coord", false], ["coord", false], ["color", true]], ret: "void", c: "gt_p8_rectfill" },
  circfill: { params: [["coord", false], ["coord", false], ["coord", false], ["color", true]], ret: "void", c: "gt_p8_circfill" },
  print:    { params: [["str", false], ["coord", true], ["coord", true], ["color", true]], ret: "void", c: "gt_p8_print" },
  btn:      { params: [["int", false]], ret: "bool", c: "gt_p8_btn" },
  min:      { params: [["num", false], ["num", false]], ret: "same", special: "min" },
  flr:      { params: [["num", false]], ret: "int", special: "flr" },
};
const CALLBACKS = ["_init", "_update", "_update60", "_draw"];
const P8_PALETTE = [0,169,90,219,51,3,6,7,91,62,31,254,190,140,94,47];

// Test target DESCRIPTORS. luacretro holds no console table - the SDK supplies
// { caps, harness }. These minimal descriptors mirror the five real SDKs enough
// to exercise every seam (real byte-identity is gated in each SDK's golden-c).
function desc({ prefix, caps, harness }) {
  return {
    caps: {
      zpFastcall: false, zpUserFn: true, fixedZp: false, banked: false,
      nativeDiv: false, colorBake: false, framebuffer: true, finalRename: false,
      prefix, ...caps,
    },
    harness: {
      signature: "void main(void)", init: [`${prefix || "gt"}_init`],
      onAudio: null, onMusic: null, onFps30: null,
      loopTop: [`${prefix || "gt"}_vsync`], frameEnd: `${prefix || "gt"}_endframe`,
      fps30Style: "runtime", returns: false, includes: [`${prefix || "gt"}_api.h`],
      ...harness,
    },
  };
}
const TARGETS = {
  gametank: desc({ prefix: "", caps: { zpFastcall: true, zpUserFn: true, fixedZp: true, banked: true, colorBake: true },
                   harness: { init: ["gt_init"], loopTop: ["gt_update_inputs"], frameEnd: "gt_endframe", includes: ["gt_api.h"] } }),
  gba:      desc({ prefix: "gba", caps: { nativeDiv: true }, harness: { signature: "int main(void)", returns: true } }),
  md:       desc({ prefix: "md", caps: { zpUserFn: false, nativeDiv: true, finalRename: true },
                   harness: { signature: "int main(bool hard)", voidArg: "(void)hard;", returns: true,
                              fps30Style: "oddCounter", oddVar: "_md_odd", includes: ["md_api.h", "md_math.h"] } }),
  nes:      desc({ prefix: "nes", caps: { zpFastcall: true, zpUserFn: true, fixedZp: true, colorBake: true, framebuffer: false, finalRename: true },
                   harness: { init: ["nes_init"], loopTop: ["nes_update_inputs", "nes_oam_clear"],
                              fps30Style: "oddCounter", oddVar: "_nes_odd", oddDeclFirst: true, includes: ["nes_api.h", "nes_math.h"] } }),
  c64:      desc({ prefix: "c64", caps: { zpUserFn: false, colorBake: true, finalRename: true },
                   harness: { init: ["c64_init"], loopTop: ["c64_update_inputs"], includes: ["c64_api.h"] } }),
};

const CORPUS = {
  hello: `function _draw() cls(1) print("hi",4,4,7) circfill(64,64,10,10) end`,
  fixedmath: `local x=0.0\nfunction _update() x+=0.5 x=x/3 x%=2 end\nfunction _draw() end`,
  fornum: `function _update() for i=0,10 do end for j=1,20,2 do end end\nfunction _draw() end`,
  cond: `local n=0\nfunction _update() if btn(0) then n+=1 end end\nfunction _draw() if n > 5 then cls(8) else cls(1) end end`,
  func: `function add(a,b) return a+b end\nfunction _update() local z=add(1,2) end\nfunction _draw() end`,
  color: `function _draw() cls(1) rectfill(0,0,10,10,8) end`,
};

// All five targets compile the corpus. The 6502 targets (gametank/nes/c64) and
// the framebuffer targets (gba/md) share the front-end; the seams differ.
for (const tname of ["gametank", "gba", "md", "nes", "c64"]) {
  const opts = { target: TARGETS[tname], sdkName: "luacretro", builtins: CORE, callbacks: CALLBACKS, p8Palette: P8_PALETTE };
  for (const [name, src] of Object.entries(CORPUS)) {
    test(`${name} compiles for ${tname}`, () => {
      const r = compile(src, `${name}.lua`, opts);
      assert.ok(r.ok, "compile failed:\n" + (r.diagnostics || []).map(d => d.message).join("\n"));
      assert.ok(r.c.includes("main("), "should emit a main()");
    });
  }
}

// The 6502 targets bake color like gametank; nes/c64 rename to their own schema.
test("nes bakes color + renames to nes_ schema", () => {
  const r = compile(`function _draw() cls(1) rectfill(0,0,10,10,8) end`, "t.lua",
    { target: TARGETS.nes, sdkName: "neslua", builtins: CORE, callbacks: CALLBACKS, p8Palette: P8_PALETTE });
  assert.ok(r.ok, (r.diagnostics || []).map(d => d.message).join("\n"));
  assert.match(r.c, /nes_cls\(169\)/);      // P8 index 1 -> baked byte, nes_ prefix
  assert.match(r.c, /#include "nes_api.h"/);
  assert.doesNotMatch(r.c, /\bgt_/);         // no gt_ symbols leak through
});

test("c64 bakes color + renames to c64_ schema + native div", () => {
  const r = compile(`local a=0\nfunction _update() a=a\\3 end\nfunction _draw() cls(1) end`, "t.lua",
    { target: TARGETS.c64, sdkName: "c64lua", builtins: CORE, callbacks: CALLBACKS, p8Palette: P8_PALETTE });
  assert.ok(r.ok, (r.diagnostics || []).map(d => d.message).join("\n"));
  assert.match(r.c, /#include "c64_api.h"/);
  assert.doesNotMatch(r.c, /\bgt_/);
});

// per-target static-allocation limits (opts.limits) tighten diagnostics only.
test("opts.limits tightens array/pool caps", () => {
  const src = `local a=array(200)\nfunction _update() end\nfunction _draw() end`;
  const wide = compile(src, "t.lua", { target: TARGETS.nes, builtins: CORE, callbacks: CALLBACKS });
  assert.ok(wide.ok);
  const tight = compile(src, "t.lua", { target: TARGETS.nes, builtins: CORE, callbacks: CALLBACKS, limits: { arrayMax: 128 } });
  assert.ok(!tight.ok);
  assert.match(tight.diagnostics.map(d => d.message).join("\n"), /between 1 and 128/);
});

// Target seams fire distinctly.
test("gametank bakes color; gba/md pass raw", () => {
  const src = `function _draw() cls(1) end`;
  const gt = compile(src, "t.lua", { target: TARGETS.gametank, builtins: CORE, callbacks: CALLBACKS, p8Palette: P8_PALETTE });
  const gba = compile(src, "t.lua", { target: TARGETS.gba, builtins: CORE, callbacks: CALLBACKS });
  assert.match(gt.c, /gt_p8_cls\(169\)/);   // P8 index 1 -> CAPTURE 169
  assert.match(gba.c, /gba_cls\(1\)/);       // raw index
});

test("md harness is main(bool hard) + md_ includes", () => {
  const r = compile(`function _draw() cls(1) end`, "t.lua", { target: TARGETS.md, builtins: CORE, callbacks: CALLBACKS });
  assert.match(r.c, /int main\(bool hard\)/);
  assert.match(r.c, /#include "md_api.h"/);
  assert.match(r.c, /md_cls/);
});

test("sdkName threads into diagnostics", () => {
  // assigning an undeclared global inside a function -> the sdkName message
  const r = compile(`function _update() y = 5 end\nfunction _draw() end`, "t.lua",
    { target: TARGETS.gba, sdkName: "gbalua", builtins: CORE, callbacks: CALLBACKS });
  assert.ok(!r.ok);
  assert.match(r.diagnostics.map(d => d.message).join("\n"), /gbalua has no implicit globals/);
});
