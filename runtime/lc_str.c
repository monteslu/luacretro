/* luacretro runtime: interned strings, number <-> string conversion.
 *
 * Every string is interned (one object per distinct byte sequence), so
 * string equality and table-key lookup are pointer compares. The intern
 * table is an open-addressing array of pointers held in the heap and
 * treated as weak by the collector. Compile-time literals are const data
 * (LC_STRLIT) registered with lc_str_intern_static() before the cart runs.
 */
#include "lc.h"
#include "lc_int.h"

/* Lua 5.2 luaS_hash, unchanged by z8lua. */
uint32_t lc_hash(const char *str, uint32_t l, uint32_t seed) {
  uint32_t h = seed ^ l;
  uint32_t step = (l >> 5) + 1;
  for (uint32_t l1 = l; l1 >= step; l1 -= step)
    h = h ^ ((h << 5) + (h >> 2) + (uint8_t)str[l1 - 1]);
  return h;
}

#define TOMB ((lc_str *)(uintptr_t)1)
static lc_str **itab;        /* data of an LC_ORAW block */
static uint32_t icap, icount, itomb;

static lc_str **raw_ptrs(uint32_t n) {
  uint8_t *b = (uint8_t *)lc_alloc(LC_ORAW, 8 + (size_t)n * sizeof(lc_str *));
  return (lc_str **)(b + 8);
}

static void itab_insert(lc_str **tab, uint32_t cap, lc_str *s) {
  uint32_t i = s->hash & (cap - 1);
  while (tab[i] && tab[i] != TOMB) i = (i + 1) & (cap - 1);
  tab[i] = s;
}

static void itab_grow(void) {
  uint32_t ncap = icap ? icap * 2 : 256;
  if (icount * 2 < icap) ncap = icap;       /* mostly tombstones: rehash same size */
  lc_str **n = raw_ptrs(ncap);
  lc_str **old = itab;
  uint32_t ocap = icap;
  for (uint32_t i = 0; i < ocap; i++)
    if (old[i] && old[i] != TOMB) itab_insert(n, ncap, old[i]);
  itab = n;
  icap = ncap;
  itomb = 0;
  if (old) lc_free((uint8_t *)old - 8);
}

void lc_str_init(void) {
  itab = 0; icap = 0; icount = 0; itomb = 0;
  itab_grow();
}

void lc_str_mark_table(void) {
  if (itab) lc_gc_mark_ptr((uint8_t *)itab - 8);
}

void lc_str_sweep_intern(void) {
  for (uint32_t i = 0; i < icap; i++) {
    lc_str *s = itab[i];
    if (s && s != TOMB && !lc_gc_is_marked(s)) {
      itab[i] = TOMB;
      icount--;
      itomb++;
    }
  }
}

static lc_str *lookup(const char *s, uint32_t len, uint32_t h) {
  uint32_t i = h & (icap - 1);
  for (;;) {
    lc_str *e = itab[i];
    if (!e) return 0;
    if (e != TOMB && e->hash == h && e->len == len && lc_memcmp(e->data, s, len) == 0)
      return e;
    i = (i + 1) & (icap - 1);
  }
}

void lc_str_intern_static(const lc_str *s) {
  if (lookup(s->data, s->len, s->hash)) return;
  if ((icount + itomb + 1) * 4 > icap * 3) itab_grow();
  itab_insert(itab, icap, (lc_str *)s);
  icount++;
}

lc_str *lc_str_new(const char *s, uint32_t len) {
  uint32_t h = lc_hash(s, len, LC_HASH_SEED);
  lc_str *e = lookup(s, len, h);
  if (e) return e;
  /* the source bytes may live in the heap (e.g. a substring of another
   * string); allocation may collect, but the source is reachable from the
   * caller's stack, which is scanned conservatively. */
  lc_str *n = (lc_str *)lc_alloc(LC_OSTR, sizeof(lc_str) + len + 1);
  n->hash = h;
  n->len = len;
  lc_memcpy(n->data, s, len);
  n->data[len] = 0;
  if ((icount + itomb + 1) * 4 > icap * 3) itab_grow();
  itab_insert(itab, icap, n);
  icount++;
  return n;
}

lc_str *lc_str_cstr(const char *s) { return lc_str_new(s, (uint32_t)lc_strlen(s)); }

/* ---- numbers -------------------------------------------------------------- */

/* z8lua lua_number2str: sprintf("%1.4f", (double)n) then strip trailing
 * zeros and a trailing dot. The double is exact (bits/65536), and printf
 * rounds the exact value half-to-even. */
int lc_fmtnum(char *buf, int32_t bits) {
  int n = 0;
  uint32_t u;
  if (bits < 0) { buf[n++] = '-'; u = 0u - (uint32_t)bits; }
  else u = (uint32_t)bits;
  uint32_t ip = u >> 16;
  uint32_t scaled = (u & 0xffff) * 10000u;
  uint32_t q = scaled >> 16, r = scaled & 0xffff;
  if (r > 0x8000 || (r == 0x8000 && (q & 1))) q++;
  if (q >= 10000) { q -= 10000; ip++; }
  char tmp[8];
  int t = 0;
  do { tmp[t++] = (char)('0' + ip % 10); ip /= 10; } while (ip);
  while (t) buf[n++] = tmp[--t];
  if (q) {
    buf[n++] = '.';
    char d[4];
    for (int i = 3; i >= 0; i--) { d[i] = (char)('0' + q % 10); q /= 10; }
    int last = 3;
    while (last >= 0 && d[last] == '0') last--;
    for (int i = 0; i <= last; i++) buf[n++] = d[i];
  }
  buf[n] = 0;
  return n;
}

static int isspace_(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
static int hexval(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* z8lua lua_strany2number for base 16 / 2. */
static int parse_any(const char *s, const char *e, int base, int32_t *out, const char **end) {
  int neg = 0;
  *end = s;
  while (s < e && isspace_((uint8_t)*s)) s++;
  if (s < e && *s == '-') { neg = 1; s++; } else if (s < e && *s == '+') s++;
  if (e - s < 2 || s[0] != '0') return 0;
  if (base == 16 && s[1] != 'x' && s[1] != 'X') return 0;
  if (base == 2 && s[1] != 'b' && s[1] != 'B') return 0;
  s += 2;
  uint32_t r = 0, f = 0;
  int i = 0, fe = 0;
  for (; s < e; s++) {
    int d = hexval((uint8_t)*s);
    if (d < 0 || d >= base) break;
    r = r * (uint32_t)base + (uint32_t)d;   /* fix32 r*base+d: integer part wraps at 16 bits */
    i++;
  }
  if (s < e && *s == '.') {
    s++;
    int max = base == 2 ? 16 : 4;
    for (; s < e; s++) {
      int d = hexval((uint8_t)*s);
      if (d < 0 || d >= base) break;
      if (max > 0) { f = f * (uint32_t)base + (uint32_t)d; fe++; }
      max--;
    }
  }
  if (i == 0 && fe == 0) return 0;
  *end = s;
  int shift = base == 2 ? fe : fe * 4;
  uint32_t bits = (r << 16) | ((f << 16) >> shift);
  if (shift == 0) bits = r << 16;
  *out = neg ? (int32_t)(0u - bits) : (int32_t)bits;
  return 1;
}

/* Decimal: strtod() then fix32(double) = int32(int64(d * 65536)). Done in
 * integers: up to 14 significant digits, exact truncation. */
static int parse_dec(const char *s, const char *e, int32_t *out, const char **end) {
  *end = s;
  while (s < e && isspace_((uint8_t)*s)) s++;
  int neg = 0;
  if (s < e && *s == '-') { neg = 1; s++; } else if (s < e && *s == '+') s++;
  uint64_t m = 0;
  int digits = 0, sig = 0, e10 = 0, any = 0;
  for (; s < e && *s >= '0' && *s <= '9'; s++) {
    any = 1;
    if (m == 0 && *s == '0') continue;
    if (sig < 14) { m = m * 10 + (uint64_t)(*s - '0'); sig++; } else e10++;
    digits++;
  }
  if (s < e && *s == '.') {
    s++;
    for (; s < e && *s >= '0' && *s <= '9'; s++) {
      any = 1;
      if (m == 0 && *s == '0') { e10--; continue; }
      if (sig < 14) { m = m * 10 + (uint64_t)(*s - '0'); sig++; e10--; }
    }
  }
  if (!any) return 0;
  if (s < e && (*s == 'e' || *s == 'E')) {
    const char *p = s + 1;
    int en = 0, ex = 0, ed = 0;
    if (p < e && *p == '-') { en = 1; p++; } else if (p < e && *p == '+') p++;
    for (; p < e && *p >= '0' && *p <= '9'; p++) { if (ex < 1000) ex = ex * 10 + (*p - '0'); ed = 1; }
    if (ed) { e10 += en ? -ex : ex; s = p; }
  }
  *end = s;
  uint64_t v;
  if (m == 0) v = 0;
  else if (e10 >= 0) {
    v = m << 16;
    for (int k = 0; k < e10 && k < 20; k++) v *= 10;
  } else {
    uint64_t div = 1;
    int k = -e10;
    if (k > 19) { v = 0; goto done; }
    for (int j = 0; j < k; j++) div *= 10;
    v = (m << 16) / div;
  }
done:
  *out = neg ? (int32_t)(0u - (uint32_t)v) : (int32_t)(uint32_t)v;
  return 1;
}

static int has_char(const char *s, uint32_t len, char a, char b) {
  for (uint32_t i = 0; i < len; i++) if (s[i] == a || s[i] == b) return 1;
  return 0;
}

/* z8lua luaO_str2d: whole string must be a number (trailing space ok). */
int lc_str2num(const char *s, uint32_t len, int32_t *out) {
  const char *e = s + len, *end;
  int ok;
  if (has_char(s, len, 'n', 'N')) return 0;
  if (has_char(s, len, 'x', 'X')) ok = parse_any(s, e, 16, out, &end);
  else if (has_char(s, len, 'b', 'B')) ok = parse_any(s, e, 2, out, &end);
  else ok = parse_dec(s, e, out, &end);
  if (!ok || end == s) return 0;
  while (end < e && isspace_((uint8_t)*end)) end++;
  return end == e;
}

/* ---- tostring / concat ------------------------------------------------------ */

static const char HEX[] = "0123456789abcdef";

static int fmtaddr(char *buf, const char *kind, const void *p) {
  int n = 0;
  while (*kind) buf[n++] = *kind++;
  buf[n++] = ':'; buf[n++] = ' '; buf[n++] = '0'; buf[n++] = 'x';
  uint32_t a = (uint32_t)(uintptr_t)p;
  for (int i = 7; i >= 0; i--) buf[n++] = HEX[(a >> (i * 4)) & 15];
  buf[n] = 0;
  return n;
}

lv lc_tostring(lv v) {
  char buf[40];
  switch (LV_TAG(v)) {
    case LT_NIL: return LV_STR(lc_str_new("nil", 3));
    case LT_FALSE: return LV_STR(lc_str_new("false", 5));
    case LT_TRUE: return LV_STR(lc_str_new("true", 4));
    case LT_NUM: { int n = lc_fmtnum(buf, LV_N(v)); return LV_STR(lc_str_new(buf, (uint32_t)n)); }
    case LT_STR: return v;
    case LT_TAB: {
      lv tm = lc_gettm(v, TM_TOSTRING);
      if (tm != LV_NIL) {
        lv *b = lc_top;
        b[0] = v;
        lc_top = b + 1;
        lv r = lc_call1(tm, b, 1);
        lc_top = b;
        return r;
      }
      int n = fmtaddr(buf, "table", LV_PTR(v));
      return LV_STR(lc_str_new(buf, (uint32_t)n));
    }
    case LT_FUN: { int n = fmtaddr(buf, "function", LV_PTR(v)); return LV_STR(lc_str_new(buf, (uint32_t)n)); }
    default: { int n = fmtaddr(buf, "thread", LV_PTR(v)); return LV_STR(lc_str_new(buf, (uint32_t)n)); }
  }
}

/* Concatenate two strings/numbers into a fresh interned string. */
static int tostr_for_concat(lv v, const char **p, uint32_t *len, char *buf) {
  if (LV_TAG(v) == LT_STR) { lc_str *s = (lc_str *)LV_PTR(v); *p = s->data; *len = s->len; return 1; }
  if (LV_TAG(v) == LT_NUM) { *len = (uint32_t)lc_fmtnum(buf, LV_N(v)); *p = buf; return 1; }
  return 0;
}

lv lc_concat(lv a, lv b) {
  char ba[16], bb[16];
  const char *pa, *pb;
  uint32_t la, lb;
  if (tostr_for_concat(a, &pa, &la, ba) && tostr_for_concat(b, &pb, &lb, bb)) {
    if (la == 0 && LV_TAG(b) == LT_STR) return b;
    if (lb == 0 && LV_TAG(a) == LT_STR) return a;
    /* build in a scratch heap block, then intern */
    uint32_t n = la + lb;
    char small[128];
    char *tmp = small;
    uint8_t *blk = 0;
    if (n > sizeof small) {
      blk = (uint8_t *)lc_alloc(LC_ORAW, 8 + n);
      /* the sources may have moved? no: the heap is non-moving */
      tmp = (char *)blk + 8;
    }
    lc_memcpy(tmp, pa, la);
    lc_memcpy(tmp + la, pb, lb);
    lc_str *s = lc_str_new(tmp, n);
    if (blk) lc_free(blk);
    return LV_STR(s);
  }
  return lc_arith(LC_OPCONCAT, a, b);
}
