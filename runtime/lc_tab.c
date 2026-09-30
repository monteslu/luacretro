/* luacretro runtime: tables.
 *
 * A port of Lua 5.2's ltable.c (array part + chained scatter hash with
 * Brent's variation), keeping z8lua's number hash so that key placement,
 * pairs() order and the #t border match a z8lua-based player that uses the
 * same string-hash seed. Only the data representation differs (lv values,
 * relative node links).
 */
#include "lc.h"
#include "lc_int.h"

#define MAXBITS 26
#define MAXASIZE (1 << MAXBITS)

static const lc_node dummynode_ = { 0, 0, 0, 0 };
#define dummynode ((lc_node *)&dummynode_)
#define isdummy(n) ((n) == dummynode)

#define sizenode(t) (1u << (t)->lsizenode)
#define gnode(t, i) (&(t)->node[i])
#define gnext(n) ((n)->next ? (n) + (n)->next : (lc_node *)0)
#define setnext(n, m) ((n)->next = (m) ? (int32_t)((m) - (n)) : 0)

static inline lc_node *hashpow2(const lc_tab *t, uint32_t h) {
  return gnode(t, h & (sizenode(t) - 1));
}
static inline lc_node *hashmod(const lc_tab *t, uint32_t n) {
  return gnode(t, n % ((sizenode(t) - 1) | 1));
}

/* z8lua: luai_hashnum(i,n) (i = (n * fix32::frombits(2654435769u)).bits()) */
static lc_node *hashnum(const lc_tab *t, int32_t n) {
  int32_t i = lc_fmul(n, (int32_t)2654435769u);
  if (i < 0) {
    if ((uint32_t)i == 0u - (uint32_t)i) i = 0;
    i = -i;
  }
  return hashmod(t, (uint32_t)i);
}

static lc_node *mainposition(const lc_tab *t, lv key) {
  switch (LV_TAG(key)) {
    case LT_NUM: return hashnum(t, LV_N(key));
    case LT_STR: return hashpow2(t, ((lc_str *)LV_PTR(key))->hash);
    case LT_FALSE: return hashpow2(t, 0);
    case LT_TRUE: return hashpow2(t, 1);
    default: return hashmod(t, LV_PAY(key));
  }
}

/* index for array part, or -1: numbers with no fractional bits */
static inline int32_t arrayindex(lv key) {
  if (LV_ISNUM(key) && (LV_N(key) & 0xffff) == 0) return LV_N(key) >> 16;
  return -1;
}

static int ceillog2(uint32_t x) {
  int l = 0;
  x--;
  while (x) { l++; x >>= 1; }
  return l;
}

static lv *vec_alloc(uint32_t n) {
  uint8_t *b = (uint8_t *)lc_alloc(LC_ORAW, 8 + (size_t)n * sizeof(lv));
  return (lv *)(b + 8);
}
static void vec_free(void *p) { if (p) lc_free((uint8_t *)p - 8); }

static lc_node *nodes_alloc(uint32_t n) {
  uint8_t *b = (uint8_t *)lc_alloc(LC_ORAW, 8 + (size_t)n * sizeof(lc_node));
  return (lc_node *)(b + 8);
}

/* ---- lookup ------------------------------------------------------------------ */

lv lc_tab_getint(lc_tab *t, int32_t key) {
  if ((uint32_t)(key - 1) < t->sizearray) return t->array[key - 1];
  int32_t nk = (int32_t)((uint32_t)key << 16);
  lc_node *n = hashnum(t, nk);
  do {
    if (n->key == LV_NUM(nk)) return n->val;
    n = gnext(n);
  } while (n);
  return LV_NIL;
}

lv lc_tab_getstr(lc_tab *t, const lc_str *key) {
  lc_node *n = hashpow2(t, key->hash);
  lv k = LV_STR(key);
  do {
    if (n->key == k) return n->val;
    n = gnext(n);
  } while (n);
  return LV_NIL;
}

static lv *getslot(lc_tab *t, lv key) {
  switch (LV_TAG(key)) {
    case LT_NIL: return 0;
    case LT_NUM:
      if ((LV_N(key) & 0xffff) == 0) {
        uint32_t k = (uint32_t)(LV_N(key) >> 16);
        if (k - 1 < t->sizearray) return &t->array[k - 1];
      }
      /* fallthrough */
    default: {
      lc_node *n = mainposition(t, key);
      do {
        if (n->key == key && !(isdummy(n))) return &n->val;
        n = gnext(n);
      } while (n);
      return 0;
    }
  }
}

lv lc_tab_get(lc_tab *t, lv key) {
  switch (LV_TAG(key)) {
    case LT_STR: return lc_tab_getstr(t, (lc_str *)LV_PTR(key));
    case LT_NIL: return LV_NIL;
    case LT_NUM:
      if ((LV_N(key) & 0xffff) == 0) return lc_tab_getint(t, LV_N(key) >> 16);
      /* fallthrough */
    default: {
      lc_node *n = mainposition(t, key);
      do {
        if (n->key == key) return n->val;
        n = gnext(n);
      } while (n);
      return LV_NIL;
    }
  }
}

/* ---- traversal ---------------------------------------------------------------- */

static int findindex(lc_tab *t, lv key) {
  if (key == LV_NIL) return -1;
  int32_t i = arrayindex(key);
  if (0 < i && (uint32_t)i <= t->sizearray) return i - 1;
  lc_node *n = mainposition(t, key);
  for (;;) {
    if (n->key == key) return (int)(n - gnode(t, 0)) + (int)t->sizearray;
    n = gnext(n);
    if (!n) lc_error("invalid key to 'next'");
  }
}

int lc_tab_next(lc_tab *t, lv *key, lv *val) {
  int i = findindex(t, *key);
  for (i++; (uint32_t)i < t->sizearray; i++) {
    if (t->array[i] != LV_NIL) {
      *key = LV_INT(i + 1);
      *val = t->array[i];
      return 1;
    }
  }
  for (i -= (int)t->sizearray; (uint32_t)i < sizenode(t); i++) {
    if (gnode(t, i)->val != LV_NIL) {
      *key = gnode(t, i)->key;
      *val = gnode(t, i)->val;
      return 1;
    }
  }
  return 0;
}

/* ---- rehash -------------------------------------------------------------------- */

static int computesizes(int nums[], int *narray) {
  int i, twotoi, a = 0, na = 0, n = 0;
  for (i = 0, twotoi = 1; twotoi / 2 < *narray; i++, twotoi *= 2) {
    if (nums[i] > 0) {
      a += nums[i];
      if (a > twotoi / 2) { n = twotoi; na = a; }
    }
    if (a == *narray) break;
  }
  *narray = n;
  return na;
}

static int countint(lv key, int *nums) {
  int32_t k = arrayindex(key);
  if (0 < k && k <= MAXASIZE) { nums[ceillog2((uint32_t)k)]++; return 1; }
  return 0;
}

static int numusearray(const lc_tab *t, int *nums) {
  int lg, ttlg, ause = 0;
  uint32_t i = 1;
  for (lg = 0, ttlg = 1; lg <= MAXBITS; lg++, ttlg *= 2) {
    int lc = 0;
    uint32_t lim = (uint32_t)ttlg;
    if (lim > t->sizearray) {
      lim = t->sizearray;
      if (i > lim) break;
    }
    for (; i <= lim; i++) if (t->array[i - 1] != LV_NIL) lc++;
    nums[lg] += lc;
    ause += lc;
  }
  return ause;
}

static int numusehash(const lc_tab *t, int *nums, int *pnasize) {
  int totaluse = 0, ause = 0;
  int i = (int)sizenode(t);
  while (i--) {
    const lc_node *n = &t->node[i];
    if (n->val != LV_NIL) { ause += countint(n->key, nums); totaluse++; }
  }
  *pnasize += ause;
  return totaluse;
}

static void setarrayvector(lc_tab *t, uint32_t size) {
  lv *na = vec_alloc(size);
  uint32_t keep = t->sizearray < size ? t->sizearray : size;
  for (uint32_t i = 0; i < keep; i++) na[i] = t->array[i];
  vec_free(t->array);
  t->array = na;
  t->sizearray = size;
}

static void setnodevector(lc_tab *t, int size) {
  int lsize;
  if (size == 0) {
    t->node = dummynode;
    lsize = 0;
  } else {
    lsize = ceillog2((uint32_t)size);
    if (lsize > MAXBITS) lc_error("table overflow");
    size = 1 << lsize;
    t->node = nodes_alloc((uint32_t)size);   /* zeroed: nil keys/values, no links */
  }
  t->lsizenode = (uint8_t)lsize;
  t->lastfree = gnode(t, isdummy(t->node) ? 0 : (uint32_t)size);
  if (isdummy(t->node)) t->lastfree = dummynode;
}

static void tab_resize(lc_tab *t, int nasize, int nhsize) {
  /* the old parts are only reachable from locals here: no collection */
  lc_gc_lock(1);
  uint32_t oldasize = t->sizearray;
  int oldhsize = t->lsizenode;
  lc_node *nold = t->node;
  lv *oldarray = t->array;
  if ((uint32_t)nasize > oldasize) { setarrayvector(t, (uint32_t)nasize); oldarray = 0; }
  setnodevector(t, nhsize);
  if ((uint32_t)nasize < oldasize) {
    /* re-insert elements from the vanishing slice, then shrink */
    t->sizearray = (uint32_t)nasize;
    for (uint32_t i = (uint32_t)nasize; i < oldasize; i++)
      if (oldarray[i] != LV_NIL) lc_tab_setint(t, (int32_t)i + 1, oldarray[i]);
    lv *na = nasize ? vec_alloc((uint32_t)nasize) : 0;
    for (int i = 0; i < nasize; i++) na[i] = oldarray[i];
    vec_free(oldarray);
    t->array = na;
  }
  if (!isdummy(nold)) {
    for (int i = (1 << oldhsize) - 1; i >= 0; i--) {
      lc_node *old = nold + i;
      if (old->val != LV_NIL) lc_tab_set(t, old->key, old->val);
    }
    vec_free(nold);
  }
  lc_gc_lock(-1);
}

static void rehash(lc_tab *t, lv ek) {
  int nasize, na, i, totaluse;
  int nums[MAXBITS + 1];
  for (i = 0; i <= MAXBITS; i++) nums[i] = 0;
  nasize = numusearray(t, nums);
  totaluse = nasize;
  totaluse += numusehash(t, nums, &nasize);
  nasize += countint(ek, nums);
  totaluse++;
  na = computesizes(nums, &nasize);
  tab_resize(t, nasize, totaluse - na);
}

lc_tab *lc_tab_new(int narr, int nhash) {
  lc_tab *t = (lc_tab *)lc_alloc(LC_OTAB, sizeof(lc_tab));
  t->meta = 0;
  t->flags = 0xff;
  t->array = 0;
  t->sizearray = 0;
  setnodevector(t, 0);
  if (narr > 0 || nhash > 0) tab_resize(t, narr, nhash);
  return t;
}

static lc_node *getfreepos(lc_tab *t) {
  if (isdummy(t->node)) return 0;
  while (t->lastfree > t->node) {
    t->lastfree--;
    if (t->lastfree->key == LV_NIL) return t->lastfree;
  }
  return 0;
}

static lv *newkey(lc_tab *t, lv key) {
  lc_node *mp;
  if (key == LV_NIL) lc_error("table index is nil");
  mp = mainposition(t, key);
  if (mp->val != LV_NIL || isdummy(mp)) {
    lc_node *othern;
    lc_node *n = getfreepos(t);
    if (!n) {
      rehash(t, key);
      lv *s = getslot(t, key);
      if (s) return s;
      return newkey(t, key);
    }
    othern = mainposition(t, mp->key);
    if (othern != mp) {
      while (gnext(othern) != mp) othern = gnext(othern);
      setnext(othern, n);
      *n = *mp;
      if (mp->next) n->next = (int32_t)((mp + mp->next) - n);
      mp->next = 0;
      mp->val = LV_NIL;
    } else {
      lc_node *mn = gnext(mp);
      setnext(n, mn);
      setnext(mp, n);
      mp = n;
    }
  }
  mp->key = key;
  return &mp->val;
}

void lc_tab_set(lc_tab *t, lv key, lv val) {
  lv *s = getslot(t, key);
  /* like Lua 5.2, assigning nil to an absent key still claims a node */
  if (!s) s = newkey(t, key);
  *s = val;
}

void lc_tab_setint(lc_tab *t, int32_t key, lv val) {
  if ((uint32_t)(key - 1) < t->sizearray) { t->array[key - 1] = val; return; }
  lc_tab_set(t, LV_INT(key), val);
}

static int32_t unbound_search(lc_tab *t, uint32_t j) {
  uint32_t i = j;
  j++;
  while (lc_tab_getint(t, (int32_t)j) != LV_NIL) {
    i = j;
    j *= 2;
    if (j > 0x7fff) {
      i = 1;
      while (lc_tab_getint(t, (int32_t)i) != LV_NIL) i++;
      return (int32_t)i - 1;
    }
  }
  while (j - i > 1) {
    uint32_t m = (i + j) / 2;
    if (lc_tab_getint(t, (int32_t)m) == LV_NIL) j = m;
    else i = m;
  }
  return (int32_t)i;
}

int32_t lc_tab_len(lc_tab *t) {
  uint32_t j = t->sizearray;
  if (j > 0 && t->array[j - 1] == LV_NIL) {
    uint32_t i = 0;
    while (j - i > 1) {
      uint32_t m = (i + j) / 2;
      if (t->array[m - 1] == LV_NIL) j = m;
      else i = m;
    }
    return (int32_t)i;
  }
  if (isdummy(t->node)) return (int32_t)j;
  return unbound_search(t, j);
}

/* OP_SETLIST: t[first + i] = v[i]; grows the array part to fit first */
void lc_tab_setlist(lc_tab *t, int32_t first, const lv *v, int n) {
  int32_t last = first + n - 1;
  if (n > 0 && (uint32_t)last > t->sizearray) {
    int nsize = isdummy(t->node) ? 0 : (int)sizenode(t);
    tab_resize(t, last, nsize);
  }
  for (int i = n - 1; i >= 0; i--) lc_tab_setint(t, first + i, v[i]);
}
