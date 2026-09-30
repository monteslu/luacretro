/* luacretro runtime: builtin function declarations.
 *
 * Every builtin is an lc_fn plus a const closure (lcb_<name>_clo) so it
 * can be called directly by generated code or stored as a first-class
 * function value.
 */
#ifndef LC_LIB_H
#define LC_LIB_H
#include "lc.h"

#define LCB_DECL(name) \
  int lcb_##name(lv *B, int N, lc_clo *S); \
  extern const lc_clo lcb_##name##_clo;

#define LCB(name) \
  int lcb_##name(lv *B, int N, lc_clo *S); \
  const lc_clo lcb_##name##_clo = { { LC_OCLO, LC_FSTATIC, 0 }, 0, 0, lcb_##name, #name }; \
  int lcb_##name(lv *B, int N, lc_clo *S)

typedef struct { const char *name; const lc_clo *fn; } lc_builtin;

extern const lc_builtin lc_lib_builtins[];
extern const lc_builtin lc_p8_builtins[];

int32_t lc_argnum(lv *B, int N, int i);
int lc_pairs_fast(lv t);
int lc_ipairs_fast(lv t);
int lc_argint(lv *B, int N, int i);
lv lc_arg(lv *B, int N, int i);
int32_t lc_sin_bits(int32_t x);
int32_t lc_atan2_bits(int32_t x, int32_t y);
int32_t lc_sqrt_bits(int32_t v);

/* inline helpers used by the typed fast paths in generated code */
static inline int32_t lc_ceil(int32_t x) { return (int32_t)(0u - ((uint32_t)(0u - (uint32_t)x) & 0xffff0000u)); }
static inline int32_t lc_absn(int32_t a) { return a >= 0 ? a : ((uint32_t)a << 1 == 0 ? ~a : -a); }
static inline int32_t lc_minn(int32_t a, int32_t b) { return a < b ? a : b; }
static inline int32_t lc_maxn(int32_t a, int32_t b) { return a > b ? a : b; }
static inline int32_t lc_midn(int32_t x, int32_t y, int32_t z) {
  return x > y ? (y > z ? y : (x < z ? x : z)) : (x > z ? x : (y < z ? y : z));
}

#define LC_LIB_NAMES(X) \
  X(max) X(min) X(mid) X(flr) X(ceil) X(sin) X(cos) X(atan2) X(sqrt) X(abs) X(sgn) \
  X(band) X(bor) X(bxor) X(bnot) X(shl) X(shr) X(lshr) X(rotl) X(rotr) X(tostr) \
  X(tonum) X(chr) X(ord) X(sub) X(split) X(type) X(tostring) X(next) X(pairs) \
  X(inext) X(ipairs) X(select) X(rawget) X(rawset) X(rawequal) X(rawlen) \
  X(setmetatable) X(getmetatable) X(unpack) X(pack) X(assert) X(cocreate) \
  X(coresume) X(costatus) X(yield) X(all) X(foreach) X(count) X(add) X(del) X(deli)

LC_LIB_NAMES(LCB_DECL)

#define LC_P8_NAMES(X) \
  X(cls) X(pset) X(pget) X(color) X(line) X(tline) X(circ) X(circfill) X(oval) \
  X(ovalfill) X(rect) X(rectfill) X(rrect) X(rrectfill) X(print) X(cursor) X(spr) \
  X(sspr) X(fget) X(fset) X(sget) X(sset) X(camera) X(clip) X(mget) X(mset) X(map) \
  X(pal) X(palt) X(fillp) X(btn) X(btnp) X(time) X(stat) X(music) X(sfx) X(cstore) \
  X(memcpy) X(memset) X(peek) X(poke) X(peek2) X(poke2) X(peek4) X(poke4) X(reload) \
  X(reset) X(cartdata) X(dget) X(dset) X(printh) X(rnd) X(srand) X(flip) X(run) \
  X(extcmd) X(stop) X(menuitem) X(noop) X(serial)

LC_P8_NAMES(LCB_DECL)

#endif
