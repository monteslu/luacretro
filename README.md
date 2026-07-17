# luacretro

The shared Lua compiler front-end for the **gtlua** (GameTank / 6502),
**gbalua** (Game Boy Advance / ARM), **mdlua** (Sega Genesis / 68000),
**neslua** (NES / 6502), and **c64lua** (Commodore 64 / 6510) console SDKs.
A PICO-8-flavored, statically-typed Lua subset compiled to C.

Each SDK depends on luacretro and calls `compile(source, file, opts)` with its
platform target and its builtin tables:

```js
import { compile } from "luacretro";
const { ok, c, diagnostics, callGraph, stubs } =
  compile(src, "main.lua", { target: "gametank", sdkName: "gtlua",
                             builtins, members, callbacks });
```

## Targets and capabilities

The emitter derives every per-platform behavior from a single capability table
(`CAPS` in `compiler/emit.js`) keyed by `opts.target`:

| target     | zpFastcall | banked | nativeDiv | colorBake | framebuffer | cName |
|------------|:----------:|:------:|:---------:|:---------:|:-----------:|-------|
| `gametank` |     ✓      |   ✓    |           |     ✓     |      ✓      | `gt_` |
| `gba`      |            |        |     ✓     |           |      ✓      | `gba_`|
| `md`       |            |        |     ✓     |           |      ✓      | `md_` |
| `nes`      |     ✓      |        |           |     ✓     |             | `nes_`|
| `c64`      |     ✓      |        |           |     ✓     |      ✓      | `c64_`|

- **zpFastcall** — the 6502 zero-page fastcall ABI (draw builtins stage args in
  `gt_a*`, user fns in `gt_p*`, fixed mul/div through `fa`/`fb`).
- **banked** — GameTank FLASH2M cross-bank far-call machinery.
- **nativeDiv** — hardware integer divide/modulo (else the runtime helpers).
- **colorBake** — bake a static P8 color literal 0-15 to a raw platform palette
  byte at compile time (needs `opts.p8Palette`).
- **framebuffer** — a full pixel surface (every draw verb lands); `nes` is a
  tile/sprite machine, so its SDK enables only the verbs it can honor.

Per-target static-allocation caps come from `opts.limits`
(`{arrayMax, poolMax}`); the defaults preserve the historical gtlua numbers.

The compiler is browser-safe (no node: / Buffer) so the web IDEs bundle it
unchanged. See `SPEC.md` for the language. The Python-family sibling is
[pycretro](https://github.com/monteslu/pycretro).
