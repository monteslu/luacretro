# luacretro

The shared Lua compiler front-end for the **gtlua** (GameTank / 6502),
**gbalua** (Game Boy Advance / ARM), and **mdlua** (Sega Genesis / 68000)
console SDKs. A PICO-8-flavored, statically-typed Lua subset compiled to C.

Each SDK depends on luacretro and calls `compile(source, file, opts)` with its
platform target and its builtin tables:

```js
import { compile } from "luacretro";
const { ok, c, diagnostics, callGraph, stubs } =
  compile(src, "main.lua", { target: "gametank", sdkName: "gtlua",
                             builtins, members, callbacks });
```

The compiler is browser-safe (no node: / Buffer) so the web IDEs bundle it
unchanged. See `SPEC.md` for the language. The Python-family sibling is
[pycretro](https://github.com/monteslu/pycretro).
