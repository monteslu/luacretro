/* luacretro runtime: the Lua base library and PICO-8's math, string and
 * table helpers.
 *
 * Semantics are ported from z8lua's lpico8lib.c (sin/cos/atan2/sqrt use
 * PICO-8's own tables and algorithms) and from the zepto8 BIOS Lua that
 * PICO-8 players run for all/foreach/add/del/deli/count. Where a PICO-8
 * function reads a missing or non-numeric argument it sees 0, exactly like
 * lua_tonumber().
 */
#include "lc.h"
#include "lc_int.h"
#include "lc_lib.h"
#include "lc_trig.h"

/* ---- argument helpers --------------------------------------------------------------- */

int32_t lc_argnum(lv *B, int N, int i) {
  int32_t n;
  if (i < N && lc_tonum(B[i], &n)) return n;
  return 0;
}
int lc_argint(lv *B, int N, int i) { return lc_argnum(B, N, i) >> 16; }
lv lc_arg(lv *B, int N, int i) { return i < N ? B[i] : LV_NIL; }

static lc_tab *argtab(lv *B, int N, int i, const char *fn) {
  lv v = lc_arg(B, N, i);
  if (LV_TAG(v) != LT_TAB) {
    static char msg[64];
    const char *a = "bad argument #1 to '", *t = "' (table expected)";
    int n = 0;
    while (*a) msg[n++] = *a++;
    while (*fn && n < 40) msg[n++] = *fn++;
    while (*t) msg[n++] = *t++;
    msg[n] = 0;
    msg[20] = (char)('1' + i);
    lc_error(msg);
  }
  return (lc_tab *)LV_PTR(v);
}

/* ---- math ------------------------------------------------------------------------------- */

LCB(max) { int32_t a = lc_argnum(B, N, 0), b = lc_argnum(B, N, 1); B[0] = LV_NUM(a > b ? a : b); return 1; }
LCB(min) { int32_t a = lc_argnum(B, N, 0), b = lc_argnum(B, N, 1); B[0] = LV_NUM(a < b ? a : b); return 1; }
LCB(mid) {
  int32_t x = lc_argnum(B, N, 0), y = lc_argnum(B, N, 1), z = lc_argnum(B, N, 2);
  int32_t r = x > y ? (y > z ? y : (x < z ? x : z)) : (x > z ? x : (y < z ? y : z));
  B[0] = LV_NUM(r);
  return 1;
}
LCB(flr) { B[0] = LV_NUM((int32_t)((uint32_t)lc_argnum(B, N, 0) & 0xffff0000u)); return 1; }
LCB(ceil) {
  int32_t x = lc_argnum(B, N, 0);
  B[0] = LV_NUM((int32_t)(0u - ((uint32_t)(0u - (uint32_t)x) & 0xffff0000u)));
  return 1;
}

int32_t lc_sin_bits(int32_t x) {
  int32_t xx = (x & 0x4000) ? ~x : x;
  int32_t a = (xx & 0x3fff) + 2;
  int32_t ret = ((a >> 2) << 4) + lc_sintable[a >> 2];
  return (x & 0x8000) ? ret : -ret;
}
LCB(sin) { B[0] = LV_NUM(lc_sin_bits(lc_argnum(B, N, 0))); return 1; }
LCB(cos) { B[0] = LV_NUM(lc_sin_bits((int32_t)((uint32_t)lc_argnum(B, N, 0) - 0x4000u))); return 1; }

int32_t lc_atan2_bits(int32_t x, int32_t y) {
  int32_t bits = 0x4000;
  if (x) {
    int64_t ax = x < 0 ? -(int64_t)x : x, ay = y < 0 ? -(int64_t)y : y;
    int64_t q = (ay << 16) / ax;
    if (q > 0x10000) bits -= lc_atantable[((int64_t)1 << 32) / q >> 5];
    else bits = lc_atantable[q >> 5];
  }
  if (x < 0) bits = 0x8000 - bits;
  if (y > 0) bits = -bits & 0xffff;
  if (x && y == (int32_t)0x80000000u) bits = -bits & 0xffff;
  return bits;
}
LCB(atan2) { B[0] = LV_NUM(lc_atan2_bits(lc_argnum(B, N, 0), lc_argnum(B, N, 1))); return 1; }

int32_t lc_sqrt_bits(int32_t v) {
  int64_t root = 0, x = (int64_t)v << 16;
  if (x > 0) {
    for (int64_t a = (int64_t)1 << 46; a; a >>= 2, root >>= 1) {
      if (x >= a + root) { x -= a + root; root += a << 1; }
    }
  }
  return (int32_t)root;
}
LCB(sqrt) { B[0] = LV_NUM(lc_sqrt_bits(lc_argnum(B, N, 0))); return 1; }

LCB(abs) {
  int32_t a = lc_argnum(B, N, 0);
  B[0] = LV_NUM(a >= 0 ? a : ((uint32_t)a << 1 == 0 ? ~a : -a));
  return 1;
}
LCB(sgn) { B[0] = LV_NUM(lc_argnum(B, N, 0) >= 0 ? 0x10000 : -0x10000); return 1; }
LCB(band) { B[0] = LV_NUM(lc_argnum(B, N, 0) & lc_argnum(B, N, 1)); return 1; }
LCB(bor) { B[0] = LV_NUM(lc_argnum(B, N, 0) | lc_argnum(B, N, 1)); return 1; }
LCB(bxor) { B[0] = LV_NUM(lc_argnum(B, N, 0) ^ lc_argnum(B, N, 1)); return 1; }
LCB(bnot) { B[0] = LV_NUM(~lc_argnum(B, N, 0)); return 1; }
LCB(shl) { B[0] = LV_NUM(lc_fshl(lc_argnum(B, N, 0), lc_argnum(B, N, 1))); return 1; }
LCB(shr) { B[0] = LV_NUM(lc_fshr(lc_argnum(B, N, 0), lc_argnum(B, N, 1))); return 1; }
LCB(lshr) { B[0] = LV_NUM(lc_flshr(lc_argnum(B, N, 0), lc_argnum(B, N, 1))); return 1; }
LCB(rotl) { B[0] = LV_NUM(lc_frotl(lc_argnum(B, N, 0), lc_argnum(B, N, 1))); return 1; }
LCB(rotr) { B[0] = LV_NUM(lc_frotr(lc_argnum(B, N, 0), lc_argnum(B, N, 1))); return 1; }

/* ---- strings ------------------------------------------------------------------------------ */

static const char HEXD[] = "0123456789abcdef";
static int put_hex4(char *p, uint32_t v) {
  for (int i = 3; i >= 0; i--) p[3 - i] = HEXD[(v >> (i * 4)) & 15];
  return 4;
}

LCB(tostr) {
  char buf[24];
  int flags = 0;
  lv v = lc_arg(B, N, 0);
  if (N >= 2) {
    lv f = B[1];
    if (LV_TAG(f) == LT_TRUE) flags = 1;
    else if (LV_TAG(f) == LT_FALSE) flags = 0;
    else { int32_t n; if (lc_tonum(f, &n)) flags = (int16_t)(n >> 16); }
  }
  int n = 0;
  switch (LV_TAG(v)) {
    case LT_NIL:
      if (N == 0) { B[0] = LV_STR(lc_str_new("", 0)); return 1; }
      B[0] = LV_STR(lc_str_new("[nil]", 5));
      return 1;
    case LT_NUM: {
      uint32_t b = (uint32_t)LV_N(v);
      if (flags) {
        if ((flags & 3) == 3) {
          buf[n++] = '0'; buf[n++] = 'x';
          n += put_hex4(buf + n, b >> 16);
          n += put_hex4(buf + n, b & 0xffff);
        } else if (flags & 2) {
          int32_t s = (int32_t)b;
          uint32_t u = s < 0 ? 0u - (uint32_t)s : (uint32_t)s;
          char t[12];
          int k = 0;
          do { t[k++] = (char)('0' + u % 10); u /= 10; } while (u);
          if (s < 0) buf[n++] = '-';
          while (k) buf[n++] = t[--k];
        } else {
          buf[n++] = '0'; buf[n++] = 'x';
          n += put_hex4(buf + n, b >> 16);
          buf[n++] = '.';
          n += put_hex4(buf + n, b & 0xffff);
        }
      } else n = lc_fmtnum(buf, (int32_t)b);
      B[0] = LV_STR(lc_str_new(buf, (uint32_t)n));
      return 1;
    }
    case LT_STR: B[0] = v; return 1;
    case LT_TRUE: B[0] = LV_STR(lc_str_new("true", 4)); return 1;
    case LT_FALSE: B[0] = LV_STR(lc_str_new("false", 5)); return 1;
    case LT_TAB:
      if (lc_gettm(v, TM_TOSTRING) != LV_NIL) { B[0] = lc_tostring(v); return 1; }
      /* fallthrough */
    case LT_FUN:
      if (flags) { B[0] = lc_tostring(v); return 1; }
      /* fallthrough */
    default: {
      const char *tn = lc_typename(v);
      buf[n++] = '[';
      while (*tn) buf[n++] = *tn++;
      buf[n++] = ']';
      B[0] = LV_STR(lc_str_new(buf, (uint32_t)n));
      return 1;
    }
  }
}

static int hexdig(int c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static uint32_t strtol_(const char *s, int base) {
  /* strtol(s, NULL, base) for the tonum flag forms (no overflow clamp
   * needed: at most 8 hex digits are ever passed) */
  int neg = 0;
  while (*s == ' ' || (*s >= '\t' && *s <= '\r')) s++;
  if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
  if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
  uint32_t v = 0;
  for (;; s++) {
    int d;
    if (*s >= '0' && *s <= '9') d = *s - '0';
    else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
    else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
    else break;
    if (d >= base) break;
    v = v * (uint32_t)base + (uint32_t)d;
  }
  return neg ? 0u - v : v;
}

LCB(tonum) {
  lv v = lc_arg(B, N, 0);
  switch (LV_TAG(v)) {
    case LT_STR: {
      lc_str *s = (lc_str *)LV_PTR(v);
      int flags = N >= 2 ? (lc_argnum(B, N, 1) >> 16) : 0;
      if (flags & 1) {
        char buf[9];
        uint32_t i = 0;
        for (; i < s->len && i < 8; i++) buf[i] = hexdig((uint8_t)s->data[i]) ? s->data[i] : '0';
        buf[i] = 0;
        uint32_t bits = strtol_(buf, 16);
        if (flags & 2) bits >>= 16;
        B[0] = LV_INT(bits);
        return 1;
      }
      if (flags & 2) {
        uint32_t bits = strtol_(s->data, 10) >> 16;
        B[0] = LV_INT(bits);
        return 1;
      }
      int32_t r;
      if (!lc_str2num(s->data, s->len, &r)) return 0;
      B[0] = LV_NUM(r);
      return 1;
    }
    case LT_NUM: B[0] = v; return 1;
    case LT_TRUE: B[0] = LV_INT(1); return 1;
    case LT_FALSE: B[0] = LV_INT(0); return 1;
    default: return 0;
  }
}

LCB(chr) {
  int n = N;
  if (n > 8192) n = 8192;
  char small[64];
  small[0] = 0;
  char *s = small;
  uint8_t *blk = 0;
  if (n > (int)sizeof small) { blk = (uint8_t *)lc_alloc(LC_ORAW, 8 + (size_t)n); s = (char *)blk + 8; }
  for (int i = 0; i < n; i++) s[i] = (char)(uint8_t)(lc_argnum(B, N, i) >> 16);
  lc_str *r = lc_str_new(s, (uint32_t)n);
  if (blk) lc_free(blk);
  B[0] = LV_STR(r);
  return 1;
}

/* luaL_tolstring-ish for ord/sub: strings and numbers only */
static int tostr_arg(lv v, const char **p, uint32_t *len, char *buf) {
  if (LV_TAG(v) == LT_STR) { lc_str *s = (lc_str *)LV_PTR(v); *p = s->data; *len = s->len; return 1; }
  if (LV_TAG(v) == LT_NUM) { *len = (uint32_t)lc_fmtnum(buf, LV_N(v)); *p = buf; return 1; }
  return 0;
}

LCB(ord) {
  char buf[16];
  const char *s;
  uint32_t len;
  lv v = lc_arg(B, N, 0);
  if (v == LV_NIL || !tostr_arg(v, &s, &len, buf)) { B[0] = LV_NIL; return 1; }
  int n = 0, count = 1;
  if (N >= 3) {
    int32_t c;
    if (!lc_tonum(B[2], &c)) return 0;
    count = c >> 16;
  }
  if (N >= 2) {
    int32_t i;
    if (!lc_tonum(B[1], &i)) return 0;
    n = (i >> 16) - 1;
  }
  if (n < 0 || (uint32_t)n >= len || count < 1) return 0;
  if ((uint32_t)(n + count) > len) count = (int)len - n;
  if (B + count > lc_stack_hi) lc_error("stack overflow");
  /* s may point into buf (a number arg); copy before overwriting B */
  uint8_t tmp[16];
  const uint8_t *src = (const uint8_t *)s;
  if (s == buf) { for (int i = 0; i < 16; i++) tmp[i] = (uint8_t)buf[i]; src = tmp; }
  for (int i = 0; i < count; i++) B[i] = LV_INT(src[n + i]);
  return count;
}

static int16_t checkint(lv *B, int N, int i, const char *fn) {
  int32_t n;
  if (i >= N || !lc_tonum(B[i], &n)) {
    (void)fn;
    lc_error("bad argument to 'sub' (number expected)");
  }
  return (int16_t)(n >> 16);
}

static uint32_t posrelat(int32_t pos, uint32_t len) {
  if (pos >= 0) return (uint32_t)pos;
  if ((uint32_t)(-pos) > len) return 0;
  return len - (uint32_t)(-pos) + 1;
}

LCB(sub) {
  char buf[16];
  const char *s;
  uint32_t l;
  if (N < 1 || !tostr_arg(B[0], &s, &l, buf)) lc_error("bad argument #1 to 'sub' (string expected)");
  uint32_t start = posrelat(checkint(B, N, 1, "sub"), l);
  int32_t endi = (N >= 3 && B[2] != LV_NIL) ? checkint(B, N, 2, "sub") : -1;
  uint32_t end = posrelat(endi, l);
  if (start < 1) start = 1;
  if (end > l) end = l;
  if (start <= end) B[0] = LV_STR(lc_str_new(s + start - 1, end - start + 1));
  else B[0] = LV_STR(lc_str_new("", 0));
  return 1;
}

LCB(split) {
  if (N == 0) return 0;
  if (B[0] == LV_NIL) return 0;
  char nbuf[16];
  const char *hay;
  uint32_t hlen;
  if (!tostr_arg(B[0], &hay, &hlen, nbuf)) lc_error("bad argument #1 to 'split' (string expected)");
  lv hayv = B[0];
  int size = 0;
  int needle = ',';
  int32_t sz;
  if (N >= 2 && lc_tonum(B[1], &sz)) {          /* lua_isnumber: numeric strings too */
    size = sz >> 16;
    if (size <= 0) size = 1;
  } else if (N >= 2 && LV_TAG(B[1]) == LT_STR) {
    lc_str *ns = (lc_str *)LV_PTR(B[1]);
    needle = ns->len ? (uint8_t)ns->data[0] : 0;
  }
  int convert = N < 3 || LV_TRUTHY(B[2]);
  lc_tab *t = lc_tab_new(0, 0);
  B[0] = LV_TAB(t);                      /* root the table */
  B[1] = hayv;                           /* and the haystack */
  lc_top = B + 2;
  const char *end = hay + hlen + (!size && needle);
  int32_t count = 0;
  for (const char *p = hay; p < end;) {
    const char *next;
    if (size) next = p + size;
    else if (needle) {
      next = 0;
      for (const char *q = p; q < hay + hlen; q++) if ((uint8_t)*q == needle) { next = q; break; }
    } else next = p + 1;
    if (!next || next > hay + hlen) next = hay + hlen;
    int32_t num;
    lv item;
    if (convert && lc_str2num(p, (uint32_t)(next - p), &num)) item = LV_NUM(num);
    else item = LV_STR(lc_str_new(p, (uint32_t)(next - p)));
    lc_tab_setint(t, ++count, item);
    p = next + (!size && needle);
  }
  lc_top = B;
  return 1;
}

/* ---- base library ---------------------------------------------------------------------------- */

LCB(type) {
  if (N < 1) lc_error("bad argument #1 to 'type' (value expected)");
  B[0] = LV_STR(lc_str_cstr(lc_typename(B[0])));
  return 1;
}

LCB(tostring) { B[0] = lc_tostring(lc_arg(B, N, 0)); return 1; }

LCB(next) {
  lc_tab *t = argtab(B, N, 0, "next");
  lv k = lc_arg(B, N, 1), v;
  if (lc_tab_next(t, &k, &v)) { B[0] = k; B[1] = v; return 2; }
  B[0] = LV_NIL;
  return 1;
}

static int empty_fn(lv *B, int N, lc_clo *S) { (void)B; (void)N; (void)S; return 0; }
LC_FNLIT(empty_clo, empty_fn, "empty");

/* z8lua pairsmeta: pairs(nil) is an empty iterator (PICO-8 0.2.0), a
 * __pairs/__ipairs metamethod supplies the triple */
static int pairsmeta(lv *B, int N, int ev, const lc_clo *iter, int zero, const char *msg) {
  lv t = lc_arg(B, N, 0);
  if (t == LV_NIL) { B[0] = LV_FUN(&empty_clo); return 1; }
  lv tm = LV_TAG(t) == LT_TAB && ((lc_tab *)LV_PTR(t))->meta
    ? lc_tab_getstr(((lc_tab *)LV_PTR(t))->meta, lc_tmname[ev]) : LV_NIL;
  if (tm == LV_NIL) {
    if (LV_TAG(t) != LT_TAB) lc_error(msg);
    B[0] = LV_FUN(iter);
    B[1] = t;
    B[2] = zero ? LV_INT(0) : LV_NIL;
    return 3;
  }
  B[0] = t;
  lc_top = B + 1;
  int n = lc_call(tm, B, 1);
  for (int i = n; i < 3; i++) B[i] = LV_NIL;
  lc_top = B;
  return 3;
}

int lc_pairs_fast(lv t) {
  if (LV_TAG(t) != LT_TAB) return 0;
  lc_tab *m = ((lc_tab *)LV_PTR(t))->meta;
  return !m || lc_tab_getstr(m, lc_tmname[TM_PAIRS]) == LV_NIL;
}
int lc_ipairs_fast(lv t) {
  if (LV_TAG(t) != LT_TAB) return 0;
  lc_tab *m = ((lc_tab *)LV_PTR(t))->meta;
  return !m || lc_tab_getstr(m, lc_tmname[TM_IPAIRS]) == LV_NIL;
}

LCB(pairs) { return pairsmeta(B, N, TM_PAIRS, &lcb_next_clo, 0, "bad argument #1 to 'pairs' (table expected)"); }

LCB(inext) {
  lc_tab *t = argtab(B, N, 0, "inext");
  int32_t i = lc_arg(B, N, 1) == LV_NIL ? 0 : (int16_t)(lc_argnum(B, N, 1) >> 16);
  i++;
  lv v = lc_tab_getint(t, i);
  /* returns the top 1 value when it is nil: the nil itself ends the loop */
  if (v == LV_NIL) { B[0] = LV_NIL; return 1; }
  B[0] = LV_INT(i);
  B[1] = v;
  return 2;
}

LCB(ipairs) { return pairsmeta(B, N, TM_IPAIRS, &lcb_inext_clo, 1, "bad argument #1 to 'ipairs' (table expected)"); }

LCB(select) {
  lv n = lc_arg(B, N, 0);
  if (LV_TAG(n) == LT_STR && ((lc_str *)LV_PTR(n))->len == 1 && ((lc_str *)LV_PTR(n))->data[0] == '#') {
    B[0] = LV_INT(N - 1);
    return 1;
  }
  int32_t i = (int16_t)(lc_argnum(B, N, 0) >> 16);
  if (i < 0) i = N + i;
  else if (i > N) i = N;
  if (i < 1) lc_error("bad argument #1 to 'select' (index out of range)");
  int cnt = N - i;
  for (int k = 0; k < cnt; k++) B[k] = B[i + k];
  return cnt;
}

LCB(rawget) { lc_tab *t = argtab(B, N, 0, "rawget"); B[0] = lc_tab_get(t, lc_arg(B, N, 1)); return 1; }
LCB(rawset) {
  lc_tab *t = argtab(B, N, 0, "rawset");
  lv k = lc_arg(B, N, 1);
  if (k == LV_NIL) lc_error("table index is nil");
  lc_tab_set(t, k, lc_arg(B, N, 2));
  return 1;
}
LCB(rawequal) { B[0] = LV_BOOL(lc_arg(B, N, 0) == lc_arg(B, N, 1)); return 1; }
LCB(rawlen) {
  lv v = lc_arg(B, N, 0);
  if (LV_TAG(v) == LT_TAB) B[0] = LV_INT(lc_tab_len((lc_tab *)LV_PTR(v)));
  else if (LV_TAG(v) == LT_STR) B[0] = LV_INT(((lc_str *)LV_PTR(v))->len);
  else lc_error("table or string expected");
  return 1;
}

LCB(setmetatable) {
  lc_tab *t = argtab(B, N, 0, "setmetatable");
  lv m = lc_arg(B, N, 1);
  if (m != LV_NIL && LV_TAG(m) != LT_TAB) lc_error("bad argument #2 to 'setmetatable' (nil or table expected)");
  if (t->meta && lc_tab_getstr(t->meta, lc_tmname[TM_METATABLE]) != LV_NIL)
    lc_error("cannot change a protected metatable");
  t->meta = m == LV_NIL ? 0 : (lc_tab *)LV_PTR(m);
  return 1;
}

LCB(getmetatable) {
  lv v = lc_arg(B, N, 0);
  lc_tab *mt = LV_TAG(v) == LT_TAB ? ((lc_tab *)LV_PTR(v))->meta : LV_TAG(v) == LT_STR ? lc_string_meta : 0;
  if (!mt) { B[0] = LV_NIL; return 1; }
  lv prot = lc_tab_getstr(mt, lc_tmname[TM_METATABLE]);
  B[0] = prot != LV_NIL ? prot : LV_TAB(mt);
  return 1;
}

LCB(unpack) {
  lv tv = lc_arg(B, N, 0);
  if (LV_TAG(tv) != LT_TAB) lc_error("bad argument #1 to 'unpack' (table expected)");
  lc_tab *t = (lc_tab *)LV_PTR(tv);
  int32_t i = (N >= 2 && B[1] != LV_NIL) ? (int16_t)(lc_argnum(B, N, 1) >> 16) : 1;
  int32_t e;
  if (N >= 3 && B[2] != LV_NIL) e = (int16_t)(lc_argnum(B, N, 2) >> 16);
  else {
    lv l = lc_len(tv);
    e = LV_N(l) >> 16;
  }
  if (i > e) return 0;
  int32_t n = e - i + 1;
  if (B + n > lc_stack_hi) lc_error("too many results to unpack");
  for (int32_t k = 0; k < n; k++) B[k] = lc_tab_getint(t, i + k);
  return (int)n;
}

LCB(pack) {
  lc_tab *t = lc_tab_new(N, 1);
  lv tv = LV_TAB(t);
  for (int i = 0; i < N; i++) lc_tab_setint(t, i + 1, B[i]);
  lc_tab_set(t, LV_STR(lc_str_new("n", 1)), LV_INT(N));
  B[0] = tv;
  return 1;
}

LCB(assert) {
  if (N >= 1 && LV_TRUTHY(B[0])) return N;
  if (N >= 2 && LV_TAG(B[1]) == LT_STR) lc_error(((lc_str *)LV_PTR(B[1]))->data);
  lc_error("assertion failed!");
}

/* ---- coroutines ---------------------------------------------------------------------------------- */

LCB(cocreate) { B[0] = lc_co_create(lc_arg(B, N, 0)); return 1; }
LCB(coresume) { return lc_co_resume(B, N); }
LCB(costatus) { B[0] = lc_co_status(lc_arg(B, N, 0)); return 1; }
LCB(yield) { return lc_co_yield(B, N); }

static int32_t mid3(int32_t x, int32_t y, int32_t z) {
  return x > y ? (y > z ? y : (x < z ? x : z)) : (x > z ? x : (y < z ? y : z));
}

/* ---- table helpers (zepto8 BIOS semantics) ------------------------------------------------------ */

/* all(c): iterator that tolerates deleting the current element */
static int all_iter(lv *B, int N, lc_clo *S) {
  (void)N;
  lv c = S->up[0]->v;
  int32_t i = LV_N(S->up[1]->v) >> 16;
  lv prev = S->up[2]->v;
  if (lc_idx(c, LV_INT(i)) == prev) i++;
  int32_t n = LV_N(lc_len(c)) >> 16;
  while (i <= n && lc_idx(c, LV_INT(i)) == LV_NIL) i++;
  prev = lc_idx(c, LV_INT(i));
  S->up[1]->v = LV_INT(i);
  S->up[2]->v = prev;
  B[0] = prev;
  return 1;
}
static int all_empty(lv *B, int N, lc_clo *S) { (void)N; (void)S; (void)B; return 0; }
LC_FNLIT(all_empty_clo, all_empty, "all");

LCB(all) {
  lv c = lc_arg(B, N, 0);
  if (c == LV_NIL || LV_N(lc_len(c)) == 0) { B[0] = LV_FUN(&all_empty_clo); return 1; }
  lc_clo *it = lc_clo_new(all_iter, 3, "all");
  B[0] = LV_FUN(it);
  lc_top = B + 1;
  it->up[0] = lc_cell_new(c);
  it->up[1] = lc_cell_new(LV_INT(1));
  it->up[2] = lc_cell_new(LV_NIL);
  lc_top = B;
  return 1;
}

LCB(foreach) {
  lv c = lc_arg(B, N, 0), f = lc_arg(B, N, 1);
  if (c == LV_NIL) return 0;
  B[0] = c; B[1] = f;
  lc_top = B + 2;
  int32_t n = LV_N(lc_len(c)) >> 16;
  if (n == 0) { lc_top = B; return 0; }
  int32_t i = 1;
  lv prev = LV_NIL;
  lv *cb = B + 2;
  for (;;) {
    if (lc_idx(c, LV_INT(i)) == prev) i++;
    n = LV_N(lc_len(c)) >> 16;
    while (i <= n && lc_idx(c, LV_INT(i)) == LV_NIL) i++;
    prev = lc_idx(c, LV_INT(i));
    if (prev == LV_NIL) break;
    cb[0] = prev;
    lc_top = cb + 1;
    lc_call(f, cb, 1);
    lc_top = B + 2;
  }
  lc_top = B;
  return 0;
}

LCB(count) {
  lv c = lc_arg(B, N, 0);
  lv v = lc_arg(B, N, 1);
  int32_t cnt = 0, max = LV_N(lc_len(c)) >> 16;
  if (N < 2 || v == LV_NIL) {
    for (int32_t i = 1; i <= max; i++) if (lc_idx(c, LV_INT(i)) != LV_NIL) cnt++;
  } else {
    for (int32_t i = 1; i <= max; i++) if (lc_eqv(lc_idx(c, LV_INT(i)), v)) cnt++;
  }
  B[0] = LV_INT(cnt);
  return 1;
}

LCB(add) {
  lv c = lc_arg(B, N, 0), x = lc_arg(B, N, 1);
  /* a false collection is ignored like nil: carts on the BBS rely on
   * deli(cond and t) (FAKE-08 and zepto8 raise on #false) */
  if (!LV_TRUTHY(c)) return 0;
  int32_t n = LV_N(lc_len(c)) >> 16;
  int32_t i;
  if (N >= 3 && LV_TRUTHY(B[2])) {
    int32_t want = lc_argnum(B, N, 2);
    int32_t fi = (int32_t)((uint32_t)want & 0xffff0000u) >> 16;   /* i\1 */
    int32_t hi = n + 1;
    i = fi < 1 ? 1 : fi > hi ? hi : fi;                         /* mid(1, i\1, #c+1) */
  } else i = n + 1;
  for (int32_t j = n; j >= i; j--) lc_setindex(c, LV_INT(j + 1), lc_idx(c, LV_INT(j)));
  lc_setindex(c, LV_INT(i), x);
  B[0] = x;
  return 1;
}

LCB(del) {
  lv c = lc_arg(B, N, 0), v = lc_arg(B, N, 1);
  if (!LV_TRUTHY(c)) return 0;
  int32_t max = LV_N(lc_len(c)) >> 16;
  for (int32_t i = 1; i <= max; i++) {
    if (lc_eqv(lc_idx(c, LV_INT(i)), v)) {
      for (int32_t j = i; j <= max; j++) lc_setindex(c, LV_INT(j), lc_idx(c, LV_INT(j + 1)));
      B[0] = v;
      return 1;
    }
  }
  return 0;
}

LCB(deli) {
  lv c = lc_arg(B, N, 0);
  if (!LV_TRUTHY(c)) return 0;
  int32_t n = LV_N(lc_len(c)) >> 16;
  int32_t i;
  if (N >= 2 && LV_TRUTHY(B[1])) {
    int32_t fi = (int32_t)((uint32_t)lc_argnum(B, N, 1) & 0xffff0000u);
    i = mid3(0x10000, fi, n << 16) >> 16;                                /* mid(1,i\\1,#c) */
  } else i = n;
  lv v = lc_idx(c, LV_INT(i));
  for (int32_t j = i; j <= n; j++) lc_setindex(c, LV_INT(j), lc_idx(c, LV_INT(j + 1)));
  B[0] = v;
  return 1;
}

/* ---- closures / cells ------------------------------------------------------------------------------ */

lc_clo *lc_clo_new(lc_fn fn, int nup, const char *name) {
  lc_clo *c = (lc_clo *)lc_alloc(LC_OCLO, sizeof(lc_clo) + (size_t)nup * sizeof(lc_cell *));
  c->fn = fn;
  c->nup = (uint16_t)nup;
  c->name = name;
  return c;
}

lc_cell *lc_cell_new(lv v) {
  lc_cell *c = (lc_cell *)lc_alloc(LC_OCELL, sizeof(lc_cell));
  c->v = v;
  return c;
}

/* ---- registry ------------------------------------------------------------------------------------------ */

const lc_builtin lc_lib_builtins[] = {
  { "max", &lcb_max_clo }, { "min", &lcb_min_clo }, { "mid", &lcb_mid_clo },
  { "flr", &lcb_flr_clo }, { "ceil", &lcb_ceil_clo }, { "sin", &lcb_sin_clo },
  { "cos", &lcb_cos_clo }, { "atan2", &lcb_atan2_clo }, { "sqrt", &lcb_sqrt_clo },
  { "abs", &lcb_abs_clo }, { "sgn", &lcb_sgn_clo }, { "band", &lcb_band_clo },
  { "bor", &lcb_bor_clo }, { "bxor", &lcb_bxor_clo }, { "bnot", &lcb_bnot_clo },
  { "shl", &lcb_shl_clo }, { "shr", &lcb_shr_clo }, { "lshr", &lcb_lshr_clo },
  { "rotl", &lcb_rotl_clo }, { "rotr", &lcb_rotr_clo }, { "tostr", &lcb_tostr_clo },
  { "tonum", &lcb_tonum_clo }, { "chr", &lcb_chr_clo }, { "ord", &lcb_ord_clo },
  { "sub", &lcb_sub_clo }, { "split", &lcb_split_clo }, { "type", &lcb_type_clo },
  { "tostring", &lcb_tostring_clo }, { "next", &lcb_next_clo }, { "pairs", &lcb_pairs_clo },
  { "inext", &lcb_inext_clo }, { "ipairs", &lcb_ipairs_clo }, { "select", &lcb_select_clo },
  { "rawget", &lcb_rawget_clo }, { "rawset", &lcb_rawset_clo }, { "rawequal", &lcb_rawequal_clo },
  { "rawlen", &lcb_rawlen_clo }, { "setmetatable", &lcb_setmetatable_clo },
  { "getmetatable", &lcb_getmetatable_clo }, { "unpack", &lcb_unpack_clo },
  { "pack", &lcb_pack_clo }, { "assert", &lcb_assert_clo }, { "cocreate", &lcb_cocreate_clo },
  { "coresume", &lcb_coresume_clo }, { "costatus", &lcb_costatus_clo }, { "yield", &lcb_yield_clo },
  { "all", &lcb_all_clo }, { "foreach", &lcb_foreach_clo }, { "count", &lcb_count_clo },
  { "add", &lcb_add_clo }, { "del", &lcb_del_clo }, { "deli", &lcb_deli_clo },
  { 0, 0 },
};

void lc_lib_mark_roots(void) {}
