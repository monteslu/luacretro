/* luacretro dynamic-tier runtime: values, heap, tables, strings, calls.
 *
 * Portable C99, no libc required (see lc_libc.c). Holds NO console
 * knowledge: an SDK supplies the platform layer declared in lc_plat.h.
 *
 * Values are 64-bit: high word = tag, low word = payload. Numbers are
 * PICO-8 16.16 fixed point stored in the payload. Heap objects are stored
 * as 32-bit addresses in the payload, so the heap must live below 4 GiB
 * (always true on the 32-bit targets; the host test build maps its arena
 * low and links non-PIE).
 *
 * Table, string-hash and length semantics follow Lua 5.2 as patched by
 * z8lua (the PICO-8 dialect used by zepto8 and FAKE-08) so that pairs()
 * order and #t match a reference player run with the same hash seed.
 */
#ifndef LC_H
#define LC_H

#include <stdint.h>
#include <stddef.h>

typedef uint64_t lv;

enum {
  LT_NIL = 0, LT_FALSE, LT_TRUE, LT_NUM, LT_STR, LT_TAB, LT_FUN, LT_THR,
};

#define LV_TAG(v)      ((uint32_t)((v) >> 32))
#define LV_PAY(v)      ((uint32_t)(v))
#define LV_MK(t, p)    (((uint64_t)(uint32_t)(t) << 32) | (uint32_t)(p))
#define LV_NIL         ((lv)0)
#define LV_FALSE       LV_MK(LT_FALSE, 0)
#define LV_TRUE        LV_MK(LT_TRUE, 0)
#define LV_BOOL(b)     ((b) ? LV_TRUE : LV_FALSE)
#define LV_NUM(bits)   LV_MK(LT_NUM, (int32_t)(bits))
#define LV_INT(i)      LV_NUM((int32_t)((uint32_t)(i) << 16))
#define LV_OBJ(t, p)   LV_MK((t), (uint32_t)(uintptr_t)(p))
#define LV_PTR(v)      ((void *)(uintptr_t)LV_PAY(v))
#define LV_N(v)        ((int32_t)LV_PAY(v))
#define LV_ISNUM(v)    (LV_TAG(v) == LT_NUM)
#define LV_ISNIL(v)    ((v) == LV_NIL)
#define LV_TRUTHY(v)   (LV_TAG(v) > LT_FALSE)
#define LV_STR(p)      LV_OBJ(LT_STR, (p))
#define LV_TAB(p)      LV_OBJ(LT_TAB, (p))
#define LV_FUN(p)      LV_OBJ(LT_FUN, (p))

/* ---- heap objects ----------------------------------------------------- */

enum {
  LC_OFREE = 0, LC_OSTR, LC_OTAB, LC_OCLO, LC_OCELL, LC_OTHR,
  LC_ORAW,        /* untyped block owned by another object (table parts) */
  LC_OVEC,        /* block of lv values traced conservatively (stacks) */
};

#define LC_FMARK    0x01
#define LC_FSTATIC  0x02   /* lives outside the arena (ROM literals) */

typedef struct lc_hdr {
  uint8_t type;
  uint8_t flags;
  uint16_t gran;          /* block size in 8-byte granules (arena only) */
} lc_hdr;

typedef struct lc_str {
  lc_hdr h;
  uint32_t hash;
  uint32_t len;
  char data[];            /* len bytes + NUL */
} lc_str;

/* A compile-time string literal. Same layout as lc_str; the compiler emits
 * these as const data with the hash precomputed for LC_HASH_SEED. */
#define LC_STRLIT(name, hash, len, text) \
  static const struct { lc_hdr h; uint32_t hs; uint32_t ln; char d[(len) + 1]; } name = \
  { { LC_OSTR, LC_FSTATIC, 0 }, (hash), (len), text }

typedef struct lc_node {
  lv val;
  lv key;
  int32_t next;           /* offset to next node in chain, 0 = end */
  int32_t pad;
} lc_node;

typedef struct lc_tab {
  lc_hdr h;
  uint8_t lsizenode;
  uint8_t flags;          /* metamethod-absent cache (bit = absent) */
  uint16_t pad;
  uint32_t sizearray;
  lv *array;
  lc_node *node;
  lc_node *lastfree;
  struct lc_tab *meta;
} lc_tab;

struct lc_clo;
/* Calling convention: arguments at base[0..nargs-1] on the value stack,
 * lc_top == base + nargs on entry. The callee writes its results to
 * base[0..] and returns how many there are. */
typedef int (*lc_fn)(lv *base, int nargs, struct lc_clo *self);

typedef struct lc_cell {
  lc_hdr h;
  uint32_t pad;
  lv v;
} lc_cell;

typedef struct lc_clo {
  lc_hdr h;
  uint16_t nup;
  uint16_t pad;
  lc_fn fn;
  const char *name;       /* for error messages */
  lc_cell *up[];
} lc_clo;

/* A closure with no upvalues, emitted as const data for top-level
 * functions and builtins so they cost no heap. */
#define LC_FNLIT(name, fn, label) \
  static const lc_clo name = { { LC_OCLO, LC_FSTATIC, 0 }, 0, 0, (fn), (label) }

/* ---- runtime state ---------------------------------------------------- */

extern lv *lc_top;           /* first free value-stack slot */
extern lv *lc_stack_lo;      /* current thread's value stack bounds */
extern lv *lc_stack_hi;
extern char *lc_cstack_limit; /* lowest safe C stack address */
extern int lc_curline;         /* source line, when compiled with debug lines */

void lc_init(void *arena, size_t arena_size, lv *vstack, size_t vstack_slots);
void lc_register_roots(lv *slots, size_t count);

/* ---- allocation / gc --------------------------------------------------- */

void *lc_alloc(uint8_t type, size_t bytes);
void lc_free(void *p);
void lc_gc_collect(void);
size_t lc_gc_used(void);
size_t lc_gc_total(void);
size_t lc_gc_peak(void);        /* high-water mark of allocated bytes */
size_t lc_gc_peak_live(void);   /* largest live set seen after a collection */

/* ---- strings ----------------------------------------------------------- */

#define LC_HASH_SEED 0x5eed1234u
uint32_t lc_hash(const char *s, uint32_t len, uint32_t seed);
lc_str *lc_str_new(const char *s, uint32_t len);   /* interned */
lc_str *lc_str_cstr(const char *s);
void lc_str_intern_static(const lc_str *s);
lv lc_concat(lv a, lv b);
lv lc_tostring(lv v);        /* Lua tostring() */
int lc_fmtnum(char *buf, int32_t bits);   /* "%1.4f" trimmed, like z8lua */
int lc_str2num(const char *s, uint32_t len, int32_t *out);

/* ---- tables ------------------------------------------------------------ */

lc_tab *lc_tab_new(int narr, int nhash);
lv lc_tab_get(lc_tab *t, lv key);
lv lc_tab_getint(lc_tab *t, int32_t k);
lv lc_tab_getstr(lc_tab *t, const lc_str *k);
void lc_tab_set(lc_tab *t, lv key, lv val);
void lc_tab_setint(lc_tab *t, int32_t k, lv val);
void lc_tab_setlist(lc_tab *t, int32_t first, const lv *v, int n);
int32_t lc_tab_len(lc_tab *t);
int lc_tab_next(lc_tab *t, lv *key, lv *val);

/* ---- operations (slow paths; fast paths are inline below) ---------------- */

enum { LC_OPADD, LC_OPSUB, LC_OPMUL, LC_OPDIV, LC_OPMOD, LC_OPPOW, LC_OPIDIV,
       LC_OPUNM, LC_OPBAND, LC_OPBOR, LC_OPBXOR, LC_OPSHL, LC_OPSHR, LC_OPLSHR,
       LC_OPROTL, LC_OPROTR, LC_OPBNOT, LC_OPCONCAT, LC_OPLEN, LC_OPEQ, LC_OPLT,
       LC_OPLE, LC_OPINDEX, LC_OPNEWINDEX, LC_OPCALL };

lv lc_arith(int op, lv a, lv b);
int lc_eq(lv a, lv b);
int lc_lt(lv a, lv b);
int lc_le(lv a, lv b);
lv lc_len(lv v);
lv lc_index(lv obj, lv key);
void lc_setindex(lv obj, lv key, lv val);
lv lc_getfield(lv obj, const lc_str *key);
void lc_setfield(lv obj, const lc_str *key, lv val);
int lc_call(lv fn, lv *base, int nargs);
lv lc_call1(lv fn, lv *base, int nargs);   /* first result or nil */
int lc_tonum(lv v, int32_t *out);          /* number or numeric string */
const char *lc_typename(lv v);

_Noreturn void lc_error(const char *msg);
_Noreturn void lc_errorf(const char *fmt, const char *a, const char *b);
void lc_stack_overflow(void);

/* fixed-point helpers (exact z8lua semantics) */
int32_t lc_fmul(int32_t a, int32_t b);
int32_t lc_fdiv(int32_t a, int32_t b);
int32_t lc_fmod(int32_t a, int32_t b);
int32_t lc_fidiv(int32_t a, int32_t b);
int32_t lc_fpow(int32_t a, int32_t b);
int32_t lc_fshl(int32_t a, int32_t n);
int32_t lc_fshr(int32_t a, int32_t n);
int32_t lc_flshr(int32_t a, int32_t n);
int32_t lc_frotl(int32_t a, int32_t n);
int32_t lc_frotr(int32_t a, int32_t n);

/* ---- closures and cells -------------------------------------------------- */

lc_clo *lc_clo_new(lc_fn fn, int nup, const char *name);
lc_cell *lc_cell_new(lv v);

/* ---- coroutines ------------------------------------------------------------ */

lv lc_co_create(lv fn);
int lc_co_resume(lv *base, int nargs);      /* builtin: coresume(co, ...) */
int lc_co_yield(lv *base, int nargs);       /* builtin: yield(...) */
lv lc_co_status(lv co);
int lc_co_running(void);

/* ---- inline fast paths ---------------------------------------------------- */

static inline lv lc_add(lv a, lv b) {
  if (LV_ISNUM(a) && LV_ISNUM(b)) return LV_NUM((int32_t)((uint32_t)LV_N(a) + (uint32_t)LV_N(b)));
  return lc_arith(LC_OPADD, a, b);
}
static inline lv lc_sub(lv a, lv b) {
  if (LV_ISNUM(a) && LV_ISNUM(b)) return LV_NUM((int32_t)((uint32_t)LV_N(a) - (uint32_t)LV_N(b)));
  return lc_arith(LC_OPSUB, a, b);
}
static inline lv lc_mul(lv a, lv b) {
  if (LV_ISNUM(a) && LV_ISNUM(b)) return LV_NUM(lc_fmul(LV_N(a), LV_N(b)));
  return lc_arith(LC_OPMUL, a, b);
}
static inline lv lc_div(lv a, lv b) {
  if (LV_ISNUM(a) && LV_ISNUM(b)) return LV_NUM(lc_fdiv(LV_N(a), LV_N(b)));
  return lc_arith(LC_OPDIV, a, b);
}
static inline lv lc_mod(lv a, lv b) {
  if (LV_ISNUM(a) && LV_ISNUM(b)) return LV_NUM(lc_fmod(LV_N(a), LV_N(b)));
  return lc_arith(LC_OPMOD, a, b);
}
static inline lv lc_idiv(lv a, lv b) {
  if (LV_ISNUM(a) && LV_ISNUM(b)) return LV_NUM(lc_fidiv(LV_N(a), LV_N(b)));
  return lc_arith(LC_OPIDIV, a, b);
}
static inline lv lc_unm(lv a) {
  if (LV_ISNUM(a)) return LV_NUM((int32_t)(0u - (uint32_t)LV_N(a)));
  return lc_arith(LC_OPUNM, a, LV_NIL);
}
static inline int lc_eqv(lv a, lv b) {
  if (a == b) return 1;
  if (LV_TAG(a) != LV_TAG(b) || LV_TAG(a) != LT_TAB) return 0;
  return lc_eq(a, b);
}
static inline int lc_ltv(lv a, lv b) {
  if (LV_ISNUM(a) && LV_ISNUM(b)) return LV_N(a) < LV_N(b);
  return lc_lt(a, b);
}
static inline int lc_lev(lv a, lv b) {
  if (LV_ISNUM(a) && LV_ISNUM(b)) return LV_N(a) <= LV_N(b);
  return lc_le(a, b);
}
static inline lv lc_idx(lv o, lv k) {
  if (LV_TAG(o) == LT_TAB) {
    lc_tab *t = (lc_tab *)LV_PTR(o);
    lv r = lc_tab_get(t, k);
    if (r != LV_NIL || !t->meta) return r;
  }
  return lc_index(o, k);
}
static inline lv lc_fld(lv o, const lc_str *k) {
  if (LV_TAG(o) == LT_TAB) {
    lc_tab *t = (lc_tab *)LV_PTR(o);
    lv r = lc_tab_getstr(t, k);
    if (r != LV_NIL || !t->meta) return r;
  }
  return lc_getfield(o, k);
}

/* lua_tonumber(): numbers and numeric strings, anything else is 0 */
static inline int32_t lc_n(lv v) {
  int32_t r;
  if (LV_ISNUM(v)) return LV_N(v);
  return lc_tonum(v, &r) ? r : 0;
}
static inline void lc_seti(lv o, lv k, lv v) {
  if (LV_TAG(o) == LT_TAB && !((lc_tab *)LV_PTR(o))->meta) {
    if (k == LV_NIL) lc_error("table index is nil");
    lc_tab_set((lc_tab *)LV_PTR(o), k, v);
  } else lc_setindex(o, k, v);
}
static inline lv lc_lenv(lv v) {
  if (LV_TAG(v) == LT_TAB && !((lc_tab *)LV_PTR(v))->meta) return LV_INT(lc_tab_len((lc_tab *)LV_PTR(v)));
  return lc_len(v);
}
/* copy n values, overlapping ranges allowed */
static inline void lc_movev(lv *dst, const lv *src, int n) {
  if (dst < src) for (int i = 0; i < n; i++) dst[i] = src[i];
  else if (dst > src) for (int i = n - 1; i >= 0; i--) dst[i] = src[i];
}
/* 'for' control values must be numbers */
static inline int32_t lc_fornum(lv v, const char *what) {
  int32_t r;
  if (!lc_tonum(v, &r)) lc_error(what);
  return r;
}

/* Function-entry guard: C stack and value stack headroom. */
#define LC_ENTER(need) do { char lc__probe; \
  if (&lc__probe < lc_cstack_limit || lc_top + (need) > lc_stack_hi) lc_stack_overflow(); \
} while (0)

#endif
