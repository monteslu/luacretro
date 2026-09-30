# luacretro

[![npm version](https://img.shields.io/npm/v/luacretro.svg)](https://www.npmjs.com/package/luacretro)

The shared Lua compiler front-end for the **gtlua** (GameTank / 6502),
**gbalua** (Game Boy Advance / ARM), **mdlua** (Sega Genesis / 68000),
**neslua** (NES / 6502), and **c64lua** (Commodore 64 / 6510) console SDKs.
A PICO-8-flavored, statically-typed Lua subset that is **ahead-of-time compiled
to C** (then to a native ROM by each SDK) - there is no Lua interpreter or VM on
the console, just native machine code. The static subset (no heap, no closures,
fixed-point numbers) is what makes that whole-program AOT compilation possible.

Each SDK depends on luacretro and calls `compile(source, file, opts)` with its
platform target and its builtin tables:

```js
import { compile } from "luacretro";
const { ok, c, diagnostics, callGraph, stubs } =
  compile(src, "main.lua", { target: "gametank", sdkName: "gtlua",
                             builtins, members, callbacks });
```

## Targets and capabilities

luacretro holds no table of consoles. Each SDK passes a target descriptor
`{ caps, harness }` as `opts.target`: `caps` switches compiler behaviors,
`harness` names the SDK runtime symbols the generated frame loop calls (see
each SDK's `compiler/index.js`). The caps the static tier understands:

- **zpFastcall** / **zpUserFn** / **fixedZp**: the 6502 zero-page fastcall ABI
  (draw builtins stage args in zero page, leaf user functions take 1-5 int
  params there, fixed mul/div go through zero-page entry points).
- **banked**: GameTank FLASH2M cross-bank far-call machinery.
- **nativeDiv**: hardware integer divide/modulo (else the runtime helpers).
- **colorBake**: bake a static P8 color literal 0-15 to a raw platform palette
  byte at compile time (needs `opts.p8Palette`).
- **framebuffer**: a full pixel surface (every draw verb lands); a tile/sprite
  machine like the NES enables only the verbs it can honor.
- **prefix** / **finalRename**: the C name prefix of the SDK runtime.
- **dynamic**: route to the dynamic tier (below).

Per-target static-allocation caps come from `opts.limits`
(`{arrayMax, poolMax}`); the defaults preserve the historical gtlua numbers.

## The dynamic tier: unmodified PICO-8 carts

Alongside the static subset, luacretro compiles **the whole PICO-8 language**
(tables, closures, metatables, coroutines, varargs, `_ENV`, strings) ahead
of time to C, linked against a portable runtime in `runtime/` that includes
the PICO-8 machine (graphics, print, synth, memory map). Real carts from the
BBS build unchanged into native ROMs, still with no interpreter on the
console:

```js
import { compileCart, RUNTIME_SOURCES } from "luacretro/dyn";
const { ok, c, diagnostics } = compileCart(bytes, "game.p8.png");
```

It needs a 32-bit C target; how many carts fit depends on RAM. sync32lua,
gbalua and mdlua build PICO-8 carts with it. The 6502 SDKs stay on the
static tier. See [DYNAMIC.md](DYNAMIC.md) for the runtime, the platform
contract (`runtime/lc_plat.h`), how it is verified, and known differences.

The compiler is browser-safe (no node: / Buffer) so the web IDEs bundle it
unchanged. See `SPEC.md` for the language. The Python-family sibling is
[pycretro](https://github.com/monteslu/pycretro).
