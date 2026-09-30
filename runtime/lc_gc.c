/* luacretro runtime: arena allocator + non-moving mark-sweep collector.
 *
 * The arena is carved into 8-byte granules. Every block (live or free)
 * starts with an lc_hdr carrying its size, so the sweep walks the arena
 * linearly and coalesces neighbouring free blocks. A start bitmap marks
 * the first granule of every allocated block: that is what lets the
 * collector treat any word found on a C stack as a possible reference
 * (conservative roots) without chasing interior or stale addresses.
 *
 * Heap objects reference each other precisely (typed tracing below);
 * only the C stacks are scanned conservatively.
 */
#include "lc.h"
#include "lc_int.h"

#define GRAN 8u
#define NSMALL 32u

static uint8_t *A_lo, *A_hi;
static uint32_t A_ngran;
static uint32_t *A_bits;          /* one bit per granule: block start */

/* Free blocks keep the index (in granules from A_lo) of the next free
 * block of the same list in the word after the header; 0xffffffff ends. */
#define NOFREE 0xffffffffu
static uint32_t fl_small[NSMALL + 1];
static uint32_t fl_big;

static size_t live_bytes, alloc_since_gc, gc_threshold, peak_live, peak_after_gc;
static int in_gc, gc_locked;

void lc_gc_lock(int d) { gc_locked += d; }

typedef struct { lv *lo; size_t n; } root_range;
#define MAXROOTS 16
static root_range roots[MAXROOTS];
static int nroots;

#define GIDX(p)   ((uint32_t)(((uint8_t *)(p) - A_lo) / GRAN))
#define GPTR(i)   ((lc_hdr *)(A_lo + (size_t)(i) * GRAN))
#define NEXTF(h)  (((uint32_t *)(h))[1])

static inline void bit_set(uint32_t i) { A_bits[i >> 5] |= 1u << (i & 31); }
static inline void bit_clr(uint32_t i) { A_bits[i >> 5] &= ~(1u << (i & 31)); }
static inline int bit_get(uint32_t i) { return (A_bits[i >> 5] >> (i & 31)) & 1; }

int lc_in_arena(const void *p) {
  return (const uint8_t *)p >= A_lo && (const uint8_t *)p < A_hi;
}

static void push_free(uint32_t idx, uint32_t g) {
  lc_hdr *h = GPTR(idx);
  h->type = LC_OFREE;
  h->flags = 0;
  h->gran = (uint16_t)g;
  if (g <= NSMALL) { NEXTF(h) = fl_small[g]; fl_small[g] = idx; }
  else { NEXTF(h) = fl_big; fl_big = idx; }
}

void lc_gc_init(void *arena, size_t size) {
  uintptr_t lo = ((uintptr_t)arena + GRAN - 1) & ~(uintptr_t)(GRAN - 1);
  uintptr_t hi = ((uintptr_t)arena + size) & ~(uintptr_t)(GRAN - 1);
  /* bitmap lives at the top of the arena: n granules need n/32 words */
  uint32_t n = (uint32_t)((hi - lo) / GRAN);
  uint32_t bitwords = (n + 31) / 32;
  uint32_t bitgran = (bitwords * 4 + GRAN - 1) / GRAN;
  n -= bitgran;
  A_lo = (uint8_t *)lo;
  A_ngran = n;
  A_hi = A_lo + (size_t)n * GRAN;
  A_bits = (uint32_t *)A_hi;
  for (uint32_t i = 0; i < bitwords; i++) A_bits[i] = 0;
  for (uint32_t i = 0; i <= NSMALL; i++) fl_small[i] = NOFREE;
  fl_big = NOFREE;
  uint32_t at = 0;
  while (at < n) {
    uint32_t g = n - at > 0xffff ? 0xffff : n - at;
    push_free(at, g);
    at += g;
  }
  live_bytes = 0;
  alloc_since_gc = 0;
  peak_live = peak_after_gc = 0;
  gc_threshold = (size_t)n * GRAN / 4;
  nroots = 0;
}

void lc_register_roots(lv *slots, size_t count) {
  if (nroots < MAXROOTS) { roots[nroots].lo = slots; roots[nroots].n = count; nroots++; }
}

/* Take a block of exactly g granules from the free lists, splitting a
 * larger one if needed. Returns granule index or NOFREE. */
static uint32_t take(uint32_t g) {
  if (g <= NSMALL && fl_small[g] != NOFREE) {
    uint32_t idx = fl_small[g];
    fl_small[g] = NEXTF(GPTR(idx));
    return idx;
  }
  /* first fit in the big list */
  uint32_t *link = &fl_big;
  while (*link != NOFREE) {
    uint32_t idx = *link;
    lc_hdr *h = GPTR(idx);
    if (h->gran >= g) {
      *link = NEXTF(h);
      uint32_t rest = h->gran - g;
      if (rest) push_free(idx + g, rest);
      return idx;
    }
    link = &NEXTF(h);
  }
  /* split a larger small block */
  for (uint32_t s = g + 1; s <= NSMALL; s++) {
    if (fl_small[s] != NOFREE) {
      uint32_t idx = fl_small[s];
      fl_small[s] = NEXTF(GPTR(idx));
      push_free(idx + g, s - g);
      return idx;
    }
  }
  return NOFREE;
}

void *lc_alloc(uint8_t type, size_t bytes) {
  uint32_t g = (uint32_t)((bytes + GRAN - 1) / GRAN);
  if (g == 0) g = 1;
  if (g > 0xffff) lc_error("not enough memory");
  if (alloc_since_gc >= gc_threshold && !in_gc && !gc_locked) lc_gc_collect();
  uint32_t idx = take(g);
  if (idx == NOFREE) {
    if (!in_gc && !gc_locked) lc_gc_collect();
    idx = take(g);
    if (idx == NOFREE) lc_error("not enough memory");
  }
  lc_hdr *h = GPTR(idx);
  uint32_t *w = (uint32_t *)h;
  for (uint32_t i = 0; i < g * (GRAN / 4); i++) w[i] = 0;
  h->type = type;
  h->flags = 0;
  h->gran = (uint16_t)g;
  bit_set(idx);
  live_bytes += (size_t)g * GRAN;
  alloc_since_gc += (size_t)g * GRAN;
  if (live_bytes > peak_live) peak_live = live_bytes;
  return h;
}

void lc_free(void *p) {
  if (!p || !lc_in_arena(p)) return;
  lc_hdr *h = (lc_hdr *)p;
  uint32_t idx = GIDX(h);
  if (!bit_get(idx)) return;
  bit_clr(idx);
  live_bytes -= (size_t)h->gran * GRAN;
  push_free(idx, h->gran);
}

size_t lc_gc_used(void) { return live_bytes; }
size_t lc_gc_peak(void) { return peak_live; }
size_t lc_gc_peak_live(void) { return peak_after_gc; }
size_t lc_gc_total(void) { return (size_t)A_ngran * GRAN; }

/* ---- marking -------------------------------------------------------------- */

#define GRAYMAX 256
static lc_hdr *gray[GRAYMAX];
static int ngray, gray_overflow;

static void mark_obj(lc_hdr *o) {
  if (!o || !lc_in_arena(o)) return;          /* static data needs no mark */
  if (o->flags & LC_FMARK) return;
  o->flags |= LC_FMARK;
  if (o->type == LC_OSTR || o->type == LC_ORAW) return;   /* no children */
  if (ngray < GRAYMAX) gray[ngray++] = o;
  else gray_overflow = 1;
}

void lc_gc_mark_value(lv v) {
  if (LV_TAG(v) >= LT_STR) mark_obj((lc_hdr *)LV_PTR(v));
}

void lc_gc_mark_ptr(void *p) { mark_obj((lc_hdr *)p); }

static void mark_values(const lv *p, size_t n) {
  for (size_t i = 0; i < n; i++) lc_gc_mark_value(p[i]);
}

/* Conservative scan: any aligned 32-bit word that is the start address of
 * an allocated block keeps that block alive. */
/* 32-bit words sit on 2-byte boundaries on the 68000 (and lv values with
 * them), so the scan steps by the platform's word alignment. */
#if defined(__m68k__)
#define SCAN_STEP 2
#else
#define SCAN_STEP 4
#endif
void lc_gc_scan_range(const void *lo, const void *hi) {
  uintptr_t a = ((uintptr_t)lo + SCAN_STEP - 1) & ~(uintptr_t)(SCAN_STEP - 1);
  for (; a + 4 <= (uintptr_t)hi; a += SCAN_STEP) {
    uint32_t w = *(const uint32_t *)a;
    const uint8_t *p = (const uint8_t *)(uintptr_t)w;
    if (p < A_lo || p >= A_hi) continue;
    uint32_t off = (uint32_t)(p - A_lo);
    if (off % GRAN) continue;
    uint32_t idx = off / GRAN;
    if (bit_get(idx)) mark_obj(GPTR(idx));
  }
}

static void traverse(lc_hdr *o) {
  switch (o->type) {
    case LC_OTAB: {
      lc_tab *t = (lc_tab *)o;
      if (t->meta) mark_obj(&t->meta->h);
      if (t->array) {
        mark_obj((lc_hdr *)((uint8_t *)t->array - 8));
        mark_values(t->array, t->sizearray);
      }
      if (t->node && lc_in_arena(t->node)) {
        mark_obj((lc_hdr *)((uint8_t *)t->node - 8));
        uint32_t n = 1u << t->lsizenode;
        for (uint32_t i = 0; i < n; i++) {
          if (t->node[i].val != LV_NIL) {
            lc_gc_mark_value(t->node[i].key);
            lc_gc_mark_value(t->node[i].val);
          }
        }
      }
      break;
    }
    case LC_OCLO: {
      lc_clo *c = (lc_clo *)o;
      for (int i = 0; i < c->nup; i++) mark_obj(&c->up[i]->h);
      break;
    }
    case LC_OCELL:
      lc_gc_mark_value(((lc_cell *)o)->v);
      break;
    case LC_OTHR:
      lc_co_traverse(o);
      break;
    case LC_OVEC:
      /* conservatively scanned by the owning thread */
      break;
    default:
      break;
  }
}

static void propagate(void) {
  for (;;) {
    while (ngray) traverse(gray[--ngray]);
    if (!gray_overflow) return;
    gray_overflow = 0;
    /* rescan: re-traverse every marked object (children already marked
     * are skipped cheaply) */
    uint32_t i = 0;
    while (i < A_ngran) {
      lc_hdr *h = GPTR(i);
      if (h->type != LC_OFREE && (h->flags & LC_FMARK)) {
        traverse(h);
        while (ngray) traverse(gray[--ngray]);
      }
      i += h->gran;
    }
  }
}

static void sweep(void) {
  for (uint32_t i = 0; i <= NSMALL; i++) fl_small[i] = NOFREE;
  fl_big = NOFREE;
  live_bytes = 0;
  uint32_t i = 0;
  while (i < A_ngran) {
    lc_hdr *h = GPTR(i);
    uint32_t g = h->gran;
    if (h->type != LC_OFREE) {
      if (h->flags & LC_FMARK) {
        h->flags &= ~LC_FMARK;
        live_bytes += (size_t)g * GRAN;
        i += g;
        continue;
      }
      bit_clr(i);
      h->type = LC_OFREE;
    }
    /* coalesce the free run starting here */
    uint32_t run = g;
    uint32_t j = i + g;
    while (j < A_ngran && run < 0xffff) {
      lc_hdr *n = GPTR(j);
      if (n->type != LC_OFREE && (n->flags & LC_FMARK)) break;
      if (n->type != LC_OFREE) { bit_clr(j); n->type = LC_OFREE; }
      if (run + n->gran > 0xffff) break;
      run += n->gran;
      j += n->gran;
    }
    push_free(i, run);
    i += run;
  }
}

void lc_gc_collect(void) {
  if (in_gc) return;
  in_gc = 1;
  ngray = 0;
  gray_overflow = 0;
#if defined(__GNUC__)
  __builtin_unwind_init();          /* spill callee-saved registers */
#endif
  volatile uint32_t here = 0;
  for (int r = 0; r < nroots; r++) mark_values(roots[r].lo, roots[r].n);
  lc_rt_mark_roots();
  lc_co_mark_stacks((const void *)&here);
  propagate();
  lc_str_sweep_intern();
  sweep();
  alloc_since_gc = 0;
  if (live_bytes > peak_after_gc) peak_after_gc = live_bytes;
  gc_threshold = live_bytes > (size_t)A_ngran * GRAN / 8 ? live_bytes : (size_t)A_ngran * GRAN / 8;
  (void)here;
  in_gc = 0;
}

int lc_gc_is_marked(const void *p) {
  if (!lc_in_arena(p)) return 1;
  return (((const lc_hdr *)p)->flags & LC_FMARK) != 0;
}

/* bytes in use per object type (LC_O*), for memory reports */
void lc_gc_census(size_t *bytes_by_type, size_t *count_by_type) {
  for (int t = 0; t < 8; t++) { bytes_by_type[t] = 0; count_by_type[t] = 0; }
  uint32_t i = 0;
  while (i < A_ngran) {
    lc_hdr *h = GPTR(i);
    if (h->type != LC_OFREE && h->type < 8) {
      bytes_by_type[h->type] += (size_t)h->gran * GRAN;
      count_by_type[h->type]++;
    }
    i += h->gran;
  }
}
