# Changelog

## 0.2.0

- The dynamic tier: the whole PICO-8 language (tables, closures,
  metatables, coroutines, varargs, `_ENV`, strings) compiled ahead of time
  to C against a portable runtime (`runtime/`) with the PICO-8 machine
  (graphics, print, synth, memory map, input). `.p8` and `.p8.png` carts
  (including PXA compression) load directly. New exports: `luacretro/dyn`,
  `luacretro/dyn/*`, `luacretro/runtime/*`. See DYNAMIC.md.
- `compile()` routes targets whose `caps.dynamic` is set to the dynamic tier.
- The static tier's output is unchanged (every SDK's goldens are
  byte-identical).
- README: the targets section now describes the `{ caps, harness }`
  descriptors instead of the removed built-in CAPS table.

## 0.1.1

- Rename `lc_` runtime symbols in banked far-call stubs.
