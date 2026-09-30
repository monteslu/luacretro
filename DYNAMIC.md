# The dynamic tier: real PICO-8 carts, ahead-of-time compiled

luacretro has two tiers that share one package:

- **The static tier** (`compiler/*.js`): the PICO-8-flavored dialect in
  [SPEC.md](SPEC.md). No heap, no closures, fixed-shape tables. It targets
  anything, down to a 1 MHz 6502, and powers gtlua, neslua, c64lua and the
  dialect side of gbalua and mdlua.
- **The dynamic tier** (`compiler/dyn/`, `runtime/`): the whole PICO-8
  language, so unmodified carts from the BBS compile. A cart becomes C that
  links against a small portable runtime. There is still no interpreter and
  no bytecode on the console: every Lua function is a C function.

This document is about the dynamic tier.

## Using it

```js
import { compileCart, RUNTIME_SOURCES, RUNTIME_HEADERS } from "luacretro/dyn";

const r = compileCart(bytes, "game.p8.png");   // .p8, .p8.png (incl. PXA), or .lua
if (!r.ok) console.error(r.diagnostics);
// r.c: the generated C; r.cart.rom: the 0x4300-byte cart ROM (it is also in r.c)
```

An SDK builds `r.c` + `runtime/<RUNTIME_SOURCES>` + its own platform layer
with the target's C compiler. A target descriptor with `caps.dynamic` routes
luacretro's regular `compile()` here too.

SDKs that use it today:

| SDK | Target | Video | Audio |
|---|---|---|---|
| sync32lua | sync32 (RP2350, Cortex-M33) | 128x128 mode, 3x on HDMI | fast synth (or exact), 48 kHz |
| gbalua (`gbalua pico8`) | Game Boy Advance | 1:1 in mode 4 | fast synth on DirectSound A, 13379 Hz |
| mdlua (`mdlua pico8`) | Sega Genesis | 16x16 tiles on plane A | sequencer driving the PSG |

The 6502 SDKs (gtlua, neslua, c64lua) stay on the static tier: the runtime
needs a 32-bit C compiler with 64-bit integers (cc65 has neither) and more
RAM than those machines have for PICO-8's 32 KB address space plus a heap.

## The platform contract

`runtime/lc_plat.h` is the whole seam between the runtime and a console:
present a frame (PICO-8's 4bpp screen + screen palette) and wait for vblank,
read buttons, store 256 bytes of cartdata, seed the RNG, report the date,
log, and show a fatal error. The runtime keeps PICO-8's entire memory map in
`lc_p8mem` exactly as PICO-8 lays it out, so `peek`/`poke`/`memcpy` behave.

The platform's `main` calls:

```c
lc_p8_run(arena, arena_bytes, value_stack, value_slots, &stack_top_local, c_stack_bytes);
```

which never returns (`run()` restarts it).

Compile-time knobs an SDK can set:

- `LC_P8_MEMSIZE`: `0x10000` for PICO-8's full 64 KB (upper memory), or
  `0x8000` for the base map.
- `LC_BIGDATA`: a section attribute for the big buffers (GBA EWRAM).
- `LC_CO_REGION` / `LC_CO_VREGION`: the shared coroutine stack sizes.
- `LC_NO_FLOAT`: use the integer emulation of `tline`'s float math.
- `LC_P8SND_FAST`: the fast synth (below); `LC_P8SND_RATE` its output rate,
  `LC_P8SND_HOT` / `LC_P8SND_BSS` code and table placement,
  `LC_P8SND_ZTAB` / `LC_P8SND_ZSEG` the size of its walk tables (smaller
  only costs speed).
- `runtime/lc_p8snd_none.c` in place of `lc_p8snd.c` for silence.

## Two synthesizers

`lc_p8snd.c` is an exact fixed-point port of FAKE-08's synth, which emulates
its doubles in software: faithful, and the most expensive thing a cart does
on a console without a double-precision FPU.

Built with `-DLC_P8SND_FAST` it becomes `lc_p8snd_fast.c`: the same
sequencer, bit-exact (`stat()` for music and sfx positions agrees on every
frame checked), but it jumps between note events instead of stepping every
sample, and synthesizes from wavetables once per block. Reverb, dampen and the
note-change crossfade are dropped. It costs about 25x less per sample (on
sync32, just-one-boss drops from 38.8% of a frame to 13.3%).

For sound chips, `lc_p8_audio_advance(samples)` runs the sequencer without
synthesis and `lc_p8_audio_voices()` reports each channel's pitch, volume
and waveform; mdlua drives the Genesis PSG that way.

## What is inside

**Values** are 64-bit: a 32-bit tag and a 32-bit payload (a 16.16 number or a
heap address). **Numbers** follow z8lua exactly: 16.16 fixed point with
PICO-8's overflow, division-by-zero, modulo and shift rules; `sin`, `cos`,
`atan2` and `sqrt` use PICO-8's own tables and algorithms.

**Tables** are a port of Lua 5.2's `ltable.c` with z8lua's number hash, so
`pairs()` order and `#t` match a z8lua-based player given the same string
hash seed (for keys that are not tables or functions).

**Strings** are interned; compile-time literals are ROM data.

**The collector** is non-moving mark-sweep over one arena: heap objects are
traced precisely, C stacks conservatively (any word that points into an
allocated block keeps it).

**Coroutines** use copied stacks: they all run on one shared stack region,
and a suspended one keeps only the bytes it used (typically a few hundred),
so carts with hundreds of live coroutines fit. The context switch is a few
lines of assembly per instruction set (x86-64 for tests, ARM/Thumb, m68k).

**The PICO-8 machine** (graphics, `print` with every P8SCII control code, the
synthesizer, input, memory, RNG) is a C port of FAKE-08 (MIT), itself built
on zepto8 (WTFPL). The PICO-8 font is CC0.

**The compiler** resolves scopes and closures, infers which locals are always
numbers (those become plain `int32_t` with no tag checks), binds PICO-8 API
names lexically as PICO-8 does (they survive `_ENV` games), calls builtins
the cart never reassigns directly (with typed fast paths for the hot ones),
and keeps globals in a static array unless the cart uses `_ENV` as a value.

## How it is tested

- `test/dyn.test.js`: a semantics suite compiled with the host C compiler
  and run headless (numbers, strings, tables, closures, varargs, metatables,
  coroutines, goto, `_ENV`, GC pressure).
- A frame-by-frame differ against a deterministic build of FAKE-08 (fixed RNG
  and hash seeds, PICO-8's floor conversion of API arguments) over the
  FAKE-08 test carts and a few hundred BBS carts; kept outside this repo
  because the carts are CC BY-NC-SA.
- The SDKs build carts through romdev and compare what the emulators show
  with the same reference, through romdev's MCP server.

## Known differences

- Where FAKE-08 and PICO-8 disagree and the right answer is known, this
  follows PICO-8: API number arguments floor rather than truncate; a cart's
  `flip()` waits a full frame at 30 fps; table fields that miss do not fall
  back to globals; `poke2`/`poke4` with several values write them at
  consecutive addresses and `poke` writes every value (zepto8 behavior).
- Where a published cart depends on behavior neither reference has, the cart
  wins: `add`/`del`/`deli` ignore a `false` collection like `nil`.
- `holdframe()` is accepted and does nothing (frames are presented once per
  frame anyway).
- `pairs()` over tables whose keys are tables or functions iterates in heap
  address order, which no two implementations share.
- `^` with a fractional exponent is computed with fixed-point log/exp and may
  differ in the last bit.
- Not supported: `load()` of source or other carts, `serial()` (a no-op),
  the pause menu, and devkit mouse/keyboard. `#include` is resolved at
  compile time.

## Limits by console

Memory decides which carts run. The heap arena is about 176 KB on sync32,
120 KB on the GBA (EWRAM) and 10 KB on the Genesis, so most BBS carts fit
sync32, many fit the GBA, and only small ones fit the Genesis (its heap is small because
SGDK needs the rest of its 64 KB).

CPU decides how well they run. On sync32 most carts fit the ABI's 150 MHz
floor; heavy 3D and effect carts do not, and the console's real clock
(252 MHz) is the headroom. The GBA (16.8 MHz) and the Genesis (7.6 MHz)
run light carts at full speed and heavier ones slowed down.
