# gtlua language specification - v0.2

gtlua is a statically-compiled, PICO-8-flavored Lua dialect for the GameTank
(design rationale and roadmap: [PICO8.md](PICO8.md)). It keeps PICO-8's
surface syntax and number model over a fixed semantic core that lowers to C
and compiles through cc65 to native 65C02 code. The design rule: **where the
dialect has a wall, fail loudly at compile time with a message that says
what to write instead.** Silent divergence from PICO-8 or C behavior is a
compiler bug.

## Program structure

A program is one `.lua` file containing, at top level:

- `local name = <constant expression>` - module state
- `function name(params...) ... end` - function definitions

**Callbacks (the PICO-8 contract):** define `_update()` plus `_draw()`;
`_init()` is optional and runs once at boot. `_update()` runs your logic once
per frame at **30 fps** (the default rate the hardware sustains once a game does
real work) with a fixed timestep - no delta-time is passed; move things by a
constant each frame and the runtime paces the frame for you. `_update60()` is
the same contract at 60 fps for light carts; a cart too heavy for its chosen
rate runs in slow motion (logic is paced to the frames it can draw), so 30 is
the sane default and 60 the measured opt-in. The runtime latches inputs before
each update and ends the frame after `_draw()` (blitter drain, vsync, page
flip). Top-level statements other than declarations are errors; top-level
initializers must be compile-time constants.

## Numbers - PICO-8 16.16 fixed point

One `number` type, semantically identical to PICO-8's (verified against the
emulated hardware in `examples/mathcheck`):

- signed 32-bit, 16 integer + 16 fraction bits: −32768.0 … 32767.99998
- **overflow wraps** (two's complement); **division by zero saturates** to
  ±0x7FFF.FFFF; `abs(-32768)` saturates
- `\` (floor division) and `%` (modulo) are **floored** - `-9\2 == -5`,
  `-9%2 == 1` (sign of divisor)
- `sgn(0) == 1`; `flr` rounds toward −∞
- literals: decimal (`1.5`), hex (`0x11.4`), binary (`0b101.1`)
- trig in **turns** (1.0 = full circle) with PICO-8's screen-space
  inversion: `sin(0.25) == -1`, `cos(0.5) == -1`, `atan2(1,1) == 0.875`

**Kinds (implementation detail, invisible semantics):** the compiler infers
which variables/expressions provably stay integral and keeps them in 16-bit
C ints (fast on the 6502); everything else is a 32-bit `long`. Integer
arithmetic wraps at the same boundaries as PICO-8's 16 integer bits, `/`
always produces a fixed result, and fractional values widen any variable
they flow into (inference runs to a fixpoint across assignments, arguments,
and returns). Power-of-two `/ \ %` fold to shifts/masks - bit-exact for
16.16.

## Dialect (PICO-8 syntax)

- compound assignment: `+= -= *= /= \= %=`
- `!=` is `~=`; `\` is floor division; **`//` is a comment** (P8/C style)
- one-line shorthand (parens required, newline ends the body, no `elseif`):
  `if (cond) stmt [else stmt]` · `while (cond) stmt`
- button glyphs `⬅️ ➡️ ⬆️ ⬇️ 🅾️ ❎` are the constants 0–5
- multiple assignment `x, y = y, x` (RHS fully evaluated first)
- bitwise: `& | ^^ << >> >>>` and unary `~` (operates on all 32 bits, so it
  always produces a fixed result); `@ $ %` memory-peek operators are not
  supported
- statements: `local` (incl. lists) · assignment · `if/elseif/else` ·
  `while` · `repeat/until` · numeric `for` (limit evaluated once; constant
  nonzero step; fractional/negative steps accumulate exactly in 16.16) ·
  `break` · `return` · calls
- no implicit globals: assignment requires a prior `local`

## Booleans and conditions

`true`/`false`, comparisons, `and or not` (boolean operands only), and
`btn`/`btnp` results. **Conditions must be boolean** - `if n then` on a
number is an error with a fix-it (`n ~= 0`). PICO-8 calls 0 truthy and C
calls it falsy; gtlua refuses to guess. This also rules out the
`x = x or default` value idiom (needs `nil`, which doesn't exist here).

## Builtins (v0.2)

- **graphics** (PICO-8 signatures; colors are raw GameTank bytes - a static 0–15
  literal is baked from the PICO-8 palette at compile time, `gt.rgb` for any byte,
  no runtime `pal()`; trailing color sets the current color; camera offset applies
  to all): `cls([c])` `camera([x,y])` `color(c)`
  `pset(x,y,[c])` `rect(x0,y0,x1,y1,[c])`
  `rectfill(x0,y0,x1,y1,[c])` (corner coords, inclusive)
  `circ(x,y,r,[c])` `circfill(x,y,r,[c])` `line(x0,y0,x1,y1,[c])`
- **input**: `btn(i,[pl])` `btnp(i,[pl])` - 0=⬅️ 1=➡️ 2=⬆️ 3=⬇️ 4=🅾️(GT A)
  5=❎(GT B) 6=GT C 7=START; `btnp` auto-repeats after 15 logical frames
  then every 4 (30 fps values; doubled at 60, per PICO-8)
- **math**: `flr ceil abs sgn sqrt min max mid sin cos atan2 rnd srand`
  `t()/time()` - `min`/`max` accept one arg (second defaults 0), `rnd(x)`
  is uniform in [0,x), `rnd()` in [0,1), `t()` is seconds since boot
- **gt.***: `gt.rgb(byte)` raw GameTank color (256-color escape hatch),
  `gt.border(c)` fills the overscan ring, `gt.ticks()` frames since boot

## Cut features and their diagnostics

Each cut fails with a specific, tested diagnostic: tables (capacity-bounded
sequences land in v0.3), strings/`..`/`?`/`print` (v0.5), closures /
anonymous / nested functions, method definitions and calls, metatables,
coroutines, varargs, `goto`, `nil`, `#`, `^` exponent, generic `for ... in`,
memory peek operators.

## Generated code contract (for debugging)

Module variables are non-static C globals `gtl_<name>` (ints or longs) -
they appear in `build/<name>.lbl`, so tests assert game state by reading
RAM (the `examples/mathcheck` pattern). User functions are `static`
`gtl_<name>`. Generated C is fully parenthesized, one Lua statement per C
statement, block structure preserved.

## Roadmap (see PICO8.md §4)

v0.3 tables + `add/del/all/foreach` + `spr`/`sspr` sprites on GRAM sheets ·
v0.4 `map/mget/fget` + `sfx/music` on the audio coprocessor + `cartdata` ·
v0.5 strings + `print` + `?` · later: `require`, 2 MB multi-bank carts,
hand-tuned asm for `gt_fmul`/`gt_fdiv`.
