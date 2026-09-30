# Changelog

## 0.2.0

- The dynamic tier: the whole PICO-8 language (tables, closures,
  metatables, coroutines, varargs, `_ENV`, strings) compiled ahead of time
  to C against a portable runtime (`runtime/`) with the PICO-8 machine
  (graphics, print, synth, memory map, input). `.p8` and `.p8.png` carts
  (including PXA compression) load directly. New exports: `luacretro/dyn`,
  `luacretro/dyn/*`, `luacretro/runtime/*`. See DYNAMIC.md.
- Two synthesizers: an exact fixed-point port of FAKE-08's synth, and a fast
  engine (`-DLC_P8SND_FAST`, `runtime/lc_p8snd_fast.c`) with the same
  sequencer and `stat()` but wavetable voices, about 25x cheaper per sample.
  `lc_p8_audio_advance` / `lc_p8_audio_voices` drive a sound chip instead
  of mixing PCM.
- `#include` in carts resolves through a `resolveInclude` option.
- `poke*(a, unpack(t))` writes straight from the table, so large unpacks do
  not need value-stack space; `poke2`/`poke4` with several values write
  them at consecutive addresses (zepto8 behavior); `add`/`del`/`deli`
  ignore a `false` collection; `holdframe()` is accepted.
- Third-party notices in THIRD_PARTY.md.
- `compile()` routes targets whose `caps.dynamic` is set to the dynamic tier.
- The static tier's output is unchanged (every SDK's goldens are
  byte-identical).
- README: the targets section now describes the `{ caps, harness }`
  descriptors instead of the removed built-in CAPS table.

## 0.1.1

- Rename `lc_` runtime symbols in banked far-call stubs.
