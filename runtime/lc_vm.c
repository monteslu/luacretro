/* luacretro runtime: fixed-point arithmetic, metamethods, indexing, calls.
 *
 * Semantics follow z8lua (PICO-8's Lua dialect): 16.16 fixed-point numbers
 * with PICO-8's overflow, division and modulo rules, PICO-8 bitwise
 * operators, and z8lua's metamethod event names (including its quirks such
 * as "_idiv" and "rotr", kept so carts behave the same).
 */
#include "lc.h"
#include "lc_int.h"
#include "lc_plat.h"

/* ---- fixed point -------------------------------------------------------------- */

int32_t lc_fmul(int32_t a, int32_t b) {
  return (int32_t)(((int64_t)a * (int64_t)b) >> 16);
}

int32_t lc_fdiv(int32_t a, int32_t b) {
  if (b == 0x10000) return a;
  if (b) {
    int64_t r = ((int64_t)a * 0x10000) / b;
    int64_t ar = r < 0 ? -r : r;
    if (ar <= 0x7fffffffLL) return (int32_t)r;
  }
  return (a ^ b) >= 0 ? 0x7fffffff : (int32_t)0x80000001u;
}

static inline int32_t fabs_(int32_t a) {
  if (a >= 0) return a;
  if ((uint32_t)a << 1 == 0) return ~a;      /* abs(0x8000) = 0x7fff.ffff */
  return -a;
}

int32_t lc_fmod(int32_t a, int32_t b) {
  b = fabs_(b);
  int32_t r = b ? a % b : 0;
  return r >= 0 ? r : r + b;
}

int32_t lc_fidiv(int32_t a, int32_t b) {
  return (int32_t)((uint32_t)lc_fdiv(a, b) & 0xffff0000u);
}

int32_t lc_fshl(int32_t a, int32_t n) {
  int y = n >> 16;
  if (y < 0) return lc_flshr(a, (int32_t)((uint32_t)(-y) << 16));
  return y >= 32 ? 0 : (int32_t)((uint32_t)a << y);
}

int32_t lc_fshr(int32_t a, int32_t n) {
  int y = n >> 16;
  if (y < 0) return lc_fshl(a, (int32_t)((uint32_t)(-y) << 16));
  return a >> (y > 31 ? 31 : y);
}

int32_t lc_flshr(int32_t a, int32_t n) {
  int y = n >> 16;
  if (y < 0) return lc_fshl(a, (int32_t)((uint32_t)(-y) << 16));
  return y >= 32 ? 0 : (int32_t)((uint32_t)a >> y);
}

int32_t lc_frotl(int32_t a, int32_t n) {
  int y = (n >> 16) & 0x1f;
  if (!y) return a;
  return (int32_t)(((uint32_t)a << y) | ((uint32_t)a >> (32 - y)));
}

int32_t lc_frotr(int32_t a, int32_t n) {
  int y = (n >> 16) & 0x1f;
  if (!y) return a;
  return (int32_t)(((uint32_t)a >> y) | ((uint32_t)a << (32 - y)));
}

/* pow: integer exponents use repeated multiply/divide exactly like
 * fix32::pow(fix32,int); fractional exponents go through fixed-point
 * log2/exp2 (z8lua uses host double pow there, so results may differ in
 * the last bit). */
static int64_t flog2_q32(uint32_t x) {           /* x in 16.16, >0; result Q32.32 */
  int64_t r = 0;
  int e = 0;
  uint64_t m = x;                                /* normalise m to [2^16, 2^17) */
  while (m >= (1u << 17)) { m >>= 1; e++; }
  while (m < (1u << 16)) { m <<= 1; e--; }
  r = (int64_t)e << 32;
  uint64_t z = m << 15;                          /* Q1.31 in [1,2) */
  for (int i = 31; i >= 0; i--) {
    z = (z * z) >> 31;
    if (z >= (2ull << 31)) { z >>= 1; r += (int64_t)1 << i; }
  }
  return r;
}
static int32_t fexp2_q32(int64_t y) {            /* y Q32.32 -> 16.16 */
  int64_t ip = y >> 32;
  uint32_t fp = (uint32_t)y;
  /* 2^frac via repeated square roots table-free: use binary expansion */
  uint64_t acc;
  /* 2^f for f in [0,1): Taylor series of e^(f ln2), Q30 */
  int64_t f = (int64_t)(fp >> 2);                /* Q30 */
  static const int64_t c[] = { 1073741824, 744261118, 257941248, 59597083, 10327387, 1431680, 165394, 16377 };
  int64_t term = 1073741824, sum = 0;
  for (int k = 0; k < 8; k++) {
    sum += (c[k] * term) >> 30;
    term = (term * f) >> 30;
  }
  acc = (uint64_t)sum;                           /* Q30 */
  int shift = (int)ip + 16 - 30;
  if (shift >= 0) { if (shift > 32) return 0x7fffffff; acc <<= shift; }
  else acc >>= (-shift > 63 ? 63 : -shift);
  if (acc > 0x7fffffffu) return 0x7fffffff;
  return (int32_t)acc;
}

int32_t lc_fpow(int32_t x, int32_t y) {
  if (((uint32_t)y & 0xffff) == 0) {
    int n = y >> 16;
    int32_t res = 0x10000;
    if (n > 0) for (int i = 0; i < n; i++) res = lc_fmul(res, x);
    else for (int i = 0; i > n; i--) res = lc_fdiv(res, x);
    return res;
  }
  if (x == 0) return 0;
  if (x < 0) return 0;                           /* NaN in double -> int64 conv */
  int64_t l = flog2_q32((uint32_t)x);
  int64_t p = (l >> 16) * (int64_t)y;           /* Q32.32 * 16.16 -> Q32.32 */
  return fexp2_q32(p);
}

/* ---- type helpers --------------------------------------------------------------- */

const char *lc_typename(lv v) {
  switch (LV_TAG(v)) {
    case LT_NIL: return "nil";
    case LT_FALSE: case LT_TRUE: return "boolean";
    case LT_NUM: return "number";
    case LT_STR: return "string";
    case LT_TAB: return "table";
    case LT_FUN: return "function";
    default: return "thread";
  }
}

int lc_tonum(lv v, int32_t *out) {
  if (LV_ISNUM(v)) { *out = LV_N(v); return 1; }
  if (LV_TAG(v) == LT_STR) {
    lc_str *s = (lc_str *)LV_PTR(v);
    return lc_str2num(s->data, s->len, out);
  }
  return 0;
}

/* ---- metamethods --------------------------------------------------------------- */

static const char *const tmnames[TM_N] = {
  "__index", "__newindex", "__call", "__add", "__sub", "__mul", "__div", "__mod",
  "__pow", "_idiv", "__unm", "__concat", "__len", "__eq", "__lt", "__le",
  "__and", "__or", "__xor", "__shl", "__shr", "__lshr", "__rotl", "rotr",
  "__not", "__tostring", "__metatable", "__pairs", "__ipairs",
};
lc_str *lc_tmname[TM_N];
lc_tab *lc_string_meta;

void lc_tm_init(void) {
  for (int i = 0; i < TM_N; i++) lc_tmname[i] = lc_str_cstr(tmnames[i]);
}

lv lc_gettm(lv o, int ev) {
  lc_tab *mt = 0;
  if (LV_TAG(o) == LT_TAB) mt = ((lc_tab *)LV_PTR(o))->meta;
  else if (LV_TAG(o) == LT_STR) mt = lc_string_meta;
  if (!mt) return LV_NIL;
  return lc_tab_getstr(mt, lc_tmname[ev]);
}

static lv call_tm2(lv tm, lv a, lv b) {
  lv *base = lc_top;
  base[0] = a;
  base[1] = b;
  lc_top = base + 2;
  lv r = lc_call1(tm, base, 2);
  lc_top = base;
  return r;
}

static const int op2tm[] = {
  [LC_OPADD] = TM_ADD, [LC_OPSUB] = TM_SUB, [LC_OPMUL] = TM_MUL, [LC_OPDIV] = TM_DIV,
  [LC_OPMOD] = TM_MOD, [LC_OPPOW] = TM_POW, [LC_OPIDIV] = TM_IDIV, [LC_OPUNM] = TM_UNM,
  [LC_OPBAND] = TM_BAND, [LC_OPBOR] = TM_BOR, [LC_OPBXOR] = TM_BXOR, [LC_OPSHL] = TM_SHL,
  [LC_OPSHR] = TM_SHR, [LC_OPLSHR] = TM_LSHR, [LC_OPROTL] = TM_ROTL, [LC_OPROTR] = TM_ROTR,
  [LC_OPBNOT] = TM_BNOT, [LC_OPCONCAT] = TM_CONCAT,
};

static int32_t numop(int op, int32_t a, int32_t b) {
  switch (op) {
    case LC_OPADD: return (int32_t)((uint32_t)a + (uint32_t)b);
    case LC_OPSUB: return (int32_t)((uint32_t)a - (uint32_t)b);
    case LC_OPMUL: return lc_fmul(a, b);
    case LC_OPDIV: return lc_fdiv(a, b);
    case LC_OPMOD: return lc_fmod(a, b);
    case LC_OPPOW: return lc_fpow(a, b);
    case LC_OPIDIV: return lc_fidiv(a, b);
    case LC_OPUNM: return (int32_t)(0u - (uint32_t)a);
    case LC_OPBAND: return a & b;
    case LC_OPBOR: return a | b;
    case LC_OPBXOR: return a ^ b;
    case LC_OPSHL: return lc_fshl(a, b);
    case LC_OPSHR: return lc_fshr(a, b);
    case LC_OPLSHR: return lc_flshr(a, b);
    case LC_OPROTL: return lc_frotl(a, b);
    case LC_OPROTR: return lc_frotr(a, b);
    case LC_OPBNOT: return ~a;
    default: return 0;
  }
}

lv lc_arith(int op, lv a, lv b) {
  int32_t na, nb;
  if (op != LC_OPCONCAT) {
    int unary = op == LC_OPUNM || op == LC_OPBNOT;
    if (lc_tonum(a, &na) && (unary || lc_tonum(b, &nb))) return LV_NUM(numop(op, na, unary ? 0 : nb));
  }
  int ev = op2tm[op];
  lv tm = lc_gettm(a, ev);
  if (tm == LV_NIL) tm = lc_gettm(b, ev);
  if (tm != LV_NIL) return call_tm2(tm, a, (op == LC_OPUNM || op == LC_OPBNOT) ? a : b);
  if (op == LC_OPCONCAT) {
    lv bad = (LV_TAG(a) == LT_STR || LV_TAG(a) == LT_NUM) ? b : a;
    lc_errorf("attempt to concatenate a %s value", lc_typename(bad), 0);
  }
  lv bad = lc_tonum(a, &na) ? b : a;
  if (op >= LC_OPBAND && op <= LC_OPBNOT)
    lc_errorf("attempt to perform bitwise operation on a %s value", lc_typename(bad), 0);
  lc_errorf("attempt to perform arithmetic on a %s value", lc_typename(bad), 0);
}

int lc_eq(lv a, lv b) {
  if (a == b) return 1;
  if (LV_TAG(a) != LT_TAB || LV_TAG(b) != LT_TAB) return 0;
  lv tm1 = lc_gettm(a, TM_EQ);
  if (tm1 == LV_NIL) return 0;
  lv tm2 = lc_gettm(b, TM_EQ);
  if (tm1 != tm2) return 0;           /* Lua 5.2: same metamethod required */
  return LV_TRUTHY(call_tm2(tm1, a, b));
}

static int strcmp_(lc_str *a, lc_str *b) {
  uint32_t n = a->len < b->len ? a->len : b->len;
  int c = lc_memcmp(a->data, b->data, n);
  if (c) return c;
  return a->len < b->len ? -1 : a->len > b->len ? 1 : 0;
}

static _Noreturn void cmperror(lv a, lv b) {
  const char *t1 = lc_typename(a), *t2 = lc_typename(b);
  if (t1 == t2 || (t1[0] == t2[0] && t1[1] == t2[1] && t1[2] == t2[2]))
    lc_errorf("attempt to compare two %s values", t1, 0);
  lc_errorf("attempt to compare %s with %s", t1, t2);
}

int lc_lt(lv a, lv b) {
  if (LV_ISNUM(a) && LV_ISNUM(b)) return LV_N(a) < LV_N(b);
  if (LV_TAG(a) == LT_STR && LV_TAG(b) == LT_STR)
    return strcmp_((lc_str *)LV_PTR(a), (lc_str *)LV_PTR(b)) < 0;
  lv tm = lc_gettm(a, TM_LT);
  if (tm == LV_NIL) tm = lc_gettm(b, TM_LT);
  if (tm == LV_NIL) cmperror(a, b);
  return LV_TRUTHY(call_tm2(tm, a, b));
}

int lc_le(lv a, lv b) {
  if (LV_ISNUM(a) && LV_ISNUM(b)) return LV_N(a) <= LV_N(b);
  if (LV_TAG(a) == LT_STR && LV_TAG(b) == LT_STR)
    return strcmp_((lc_str *)LV_PTR(a), (lc_str *)LV_PTR(b)) <= 0;
  lv tm = lc_gettm(a, TM_LE);
  if (tm == LV_NIL) tm = lc_gettm(b, TM_LE);
  if (tm != LV_NIL) return LV_TRUTHY(call_tm2(tm, a, b));
  tm = lc_gettm(b, TM_LT);
  if (tm == LV_NIL) tm = lc_gettm(a, TM_LT);
  if (tm == LV_NIL) cmperror(a, b);
  return !LV_TRUTHY(call_tm2(tm, b, a));
}

lv lc_len(lv v) {
  if (LV_TAG(v) == LT_STR) return LV_INT(((lc_str *)LV_PTR(v))->len);
  if (LV_TAG(v) == LT_TAB) {
    lv tm = lc_gettm(v, TM_LEN);
    if (tm == LV_NIL) return LV_INT(lc_tab_len((lc_tab *)LV_PTR(v)));
    return call_tm2(tm, v, LV_NIL);
  }
  lv tm = lc_gettm(v, TM_LEN);
  if (tm != LV_NIL) return call_tm2(tm, v, LV_NIL);
  lc_errorf("attempt to get length of a %s value", lc_typename(v), 0);
}

/* ---- indexing ------------------------------------------------------------------- */

static lv string_index(lv s, lv k) {
  /* PICO-8: s[i] is sub(s,i,i) */
  int32_t n;
  if (!lc_tonum(k, &n)) return LV_NIL;
  lc_str *str = (lc_str *)LV_PTR(s);
  int32_t i = n >> 16;
  if (i < 0) i = (int32_t)str->len + i + 1;
  if (i < 1 || (uint32_t)i > str->len) return LV_STR(lc_str_new("", 0));
  return LV_STR(lc_str_new(str->data + i - 1, 1));
}

lv lc_index(lv o, lv k) {
  for (int loop = 0; loop < 100; loop++) {
    lv tm;
    if (LV_TAG(o) == LT_TAB) {
      lc_tab *t = (lc_tab *)LV_PTR(o);
      lv r = lc_tab_get(t, k);
      if (r != LV_NIL) return r;
      if (!t->meta) return LV_NIL;
      tm = lc_tab_getstr(t->meta, lc_tmname[TM_INDEX]);
      if (tm == LV_NIL) return LV_NIL;
    } else if (LV_TAG(o) == LT_STR) {
      if (!lc_string_meta) return string_index(o, k);
      tm = lc_tab_getstr(lc_string_meta, lc_tmname[TM_INDEX]);
      if (tm == LV_NIL) return string_index(o, k);
    } else {
      tm = lc_gettm(o, TM_INDEX);
      if (tm == LV_NIL) {
        if (LV_TAG(k) == LT_STR)
          lc_errorf("attempt to index a %s value (field '%s')", lc_typename(o), ((lc_str *)LV_PTR(k))->data);
        lc_errorf("attempt to index a %s value", lc_typename(o), 0);
      }
    }
    if (LV_TAG(tm) == LT_FUN) return call_tm2(tm, o, k);
    o = tm;
  }
  lc_error("loop in gettable");
}

void lc_setindex(lv o, lv k, lv v) {
  for (int loop = 0; loop < 100; loop++) {
    lv tm;
    if (LV_TAG(o) == LT_TAB) {
      lc_tab *t = (lc_tab *)LV_PTR(o);
      if (!t->meta || lc_tab_get(t, k) != LV_NIL ||
          (tm = lc_tab_getstr(t->meta, lc_tmname[TM_NEWINDEX])) == LV_NIL) {
        if (LV_TAG(k) == LT_NIL) lc_error("table index is nil");
        lc_tab_set(t, k, v);
        return;
      }
    } else {
      tm = lc_gettm(o, TM_NEWINDEX);
      if (tm == LV_NIL) {
        if (LV_TAG(k) == LT_STR)
          lc_errorf("attempt to index a %s value (field '%s')", lc_typename(o), ((lc_str *)LV_PTR(k))->data);
        lc_errorf("attempt to index a %s value", lc_typename(o), 0);
      }
    }
    if (LV_TAG(tm) == LT_FUN) {
      lv *base = lc_top;
      base[0] = o; base[1] = k; base[2] = v;
      lc_top = base + 3;
      lc_call(tm, base, 3);
      lc_top = base;
      return;
    }
    o = tm;
  }
  lc_error("loop in settable");
}

lv lc_getfield(lv o, const lc_str *k) { return lc_index(o, LV_STR(k)); }
void lc_setfield(lv o, const lc_str *k, lv v) {
  if (LV_TAG(o) == LT_TAB && !((lc_tab *)LV_PTR(o))->meta) {
    lc_tab_set((lc_tab *)LV_PTR(o), LV_STR(k), v);
    return;
  }
  lc_setindex(o, LV_STR(k), v);
}

/* ---- calls ------------------------------------------------------------------------ */

int lc_call(lv fn, lv *base, int nargs) {
  if (LV_TAG(fn) == LT_FUN) {
    lc_clo *c = (lc_clo *)LV_PTR(fn);
    lc_top = base + nargs;
    return c->fn(base, nargs, c);
  }
  lv tm = lc_gettm(fn, TM_CALL);
  if (tm == LV_NIL) lc_errorf("attempt to call a %s value", lc_typename(fn), 0);
  for (int i = nargs; i > 0; i--) base[i] = base[i - 1];
  base[0] = fn;
  return lc_call(tm, base, nargs + 1);
}

lv lc_call1(lv fn, lv *base, int nargs) {
  int n = lc_call(fn, base, nargs);
  return n > 0 ? base[0] : LV_NIL;
}

void lc_stack_overflow(void) { lc_error("stack overflow"); }

/* ---- errors ------------------------------------------------------------------------- */

static char errbuf[160];

_Noreturn void lc_errorf(const char *fmt, const char *a, const char *b) {
  int n = 0;
  const char *args[2] = { a, b };
  int ai = 0;
  for (const char *p = fmt; *p && n < (int)sizeof errbuf - 1; p++) {
    if (p[0] == '%' && p[1] == 's') {
      const char *s = ai < 2 && args[ai] ? args[ai] : "?";
      ai++;
      while (*s && n < (int)sizeof errbuf - 1) errbuf[n++] = *s++;
      p++;
    } else errbuf[n++] = *p;
  }
  errbuf[n] = 0;
  lc_error(errbuf);
}

/* lc_error is implemented by the coroutine module: inside a coroutine it
 * unwinds to the resumer; on the main thread it is fatal. */
