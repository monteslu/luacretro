/* luacretro runtime: coroutines, errors, runtime init.
 *
 * Coroutines use copied stacks: every coroutine runs on one shared C stack
 * region (and one shared value-stack region); switching away saves only
 * the bytes the coroutine actually uses into a heap buffer, switching in
 * copies them back to the same addresses. A suspended coroutine therefore
 * costs a few hundred bytes, not a fixed stack - carts keep hundreds of
 * them alive. The copying runs on a small scheduler stack so no code ever
 * overwrites the stack it is executing on.
 *
 * Context switching is a callee-saved-register swap (lc_ctx_swap, one small
 * routine per instruction set below). Errors unwind without setjmp: inside
 * a coroutine the error kills it and switches straight back to the resumer
 * (coresume returns false, msg); on the main thread an error is fatal and
 * goes to the platform's error screen.
 */
#include "lc.h"
#include "lc_int.h"
#include "lc_plat.h"

#ifndef LC_CO_REGION
#define LC_CO_REGION 16384       /* bytes of shared coroutine C stack */
#endif
#ifndef LC_CO_VREGION
#define LC_CO_VREGION 1024       /* slots of shared coroutine value stack */
#endif
#ifndef LC_SCHED_STACK
#define LC_SCHED_STACK 1024      /* the copying trampoline's own stack */
#endif
#define XFER_MAX 256             /* values handed across one switch */
#define CSTACK_MARGIN 768        /* headroom kept below lc_cstack_limit */

enum { CO_SUSPENDED, CO_RUNNING, CO_NORMAL, CO_DEAD };

typedef struct lc_thr {
  lc_hdr h;
  uint8_t status;
  uint8_t started;
  uint8_t failed;
  uint8_t pad;
  lv fn;
  lv err;
  void *csp;                     /* saved C stack pointer */
  lv *vtop;                      /* saved lc_top */
  uint8_t *cbuf;                 /* saved C stack bytes (LC_OVEC data) */
  uint32_t ccap, csize;
  lv *vbuf;                      /* saved value stack (LC_OVEC data) */
  uint32_t vcap, vsize;
  struct lc_thr *resumer;
} lc_thr;

/* ---- context switch --------------------------------------------------------------- */

void lc_ctx_swap(void **save_sp, void *new_sp);

#if defined(__x86_64__)
__asm__(
  ".text\n.globl lc_ctx_swap\n.type lc_ctx_swap,@function\nlc_ctx_swap:\n"
  "  pushq %rbp\n  pushq %rbx\n  pushq %r12\n  pushq %r13\n  pushq %r14\n  pushq %r15\n"
  "  movq %rsp, (%rdi)\n  movq %rsi, %rsp\n"
  "  popq %r15\n  popq %r14\n  popq %r13\n  popq %r12\n  popq %rbx\n  popq %rbp\n"
  "  ret\n");
#define CTX_WORDS 6
#elif defined(__arm__) && defined(__thumb__)
/* Thumb-1 encodings only, so the same code runs on ARMv4T (ARM7TDMI) and
 * ARMv8-M; high registers go through r4-r7. */
#if defined(__ARM_FP) && (__ARM_ARCH >= 7)
#define CTX_FPSAVE "  vpush {s16-s31}\n"
#define CTX_FPLOAD "  vpop {s16-s31}\n"
#define CTX_FPWORDS 16
#else
#define CTX_FPSAVE ""
#define CTX_FPLOAD ""
#define CTX_FPWORDS 0
#endif
__asm__(
  ".text\n.align 2\n.globl lc_ctx_swap\n.thumb\n.thumb_func\n.type lc_ctx_swap,%function\nlc_ctx_swap:\n"
  "  push {r4-r7, lr}\n"
  "  mov r4, r8\n  mov r5, r9\n  mov r6, r10\n  mov r7, r11\n"
  "  push {r4-r7}\n"
  CTX_FPSAVE
  "  mov r2, sp\n  str r2, [r0]\n  mov sp, r1\n"
  CTX_FPLOAD
  "  pop {r4-r7}\n"
  "  mov r8, r4\n  mov r9, r5\n  mov r10, r6\n  mov r11, r7\n"
  "  pop {r4-r7}\n"
  "  pop {r2}\n  bx r2\n");
#define CTX_WORDS (CTX_FPWORDS + 8)
#elif defined(__arm__)
__asm__(
  ".text\n.align 2\n.globl lc_ctx_swap\n.arm\n.type lc_ctx_swap,%function\nlc_ctx_swap:\n"
  "  stmfd sp!, {r4-r11, lr}\n"
  "  str sp, [r0]\n  mov sp, r1\n"
  "  ldmfd sp!, {r4-r11, lr}\n"
  "  bx lr\n");
#define CTX_WORDS 8
#elif defined(__m68k__)
__asm__(
  ".text\n.even\n.globl lc_ctx_swap\nlc_ctx_swap:\n"
  "  move.l 4(%sp),%a0\n  move.l 8(%sp),%a1\n"
  "  movem.l %d2-%d7/%a2-%a6,-(%sp)\n"
  "  move.l %sp,(%a0)\n  move.l %a1,%sp\n"
  "  movem.l (%sp)+,%d2-%d7/%a2-%a6\n"
  "  rts\n");
#define CTX_WORDS 11
#else
#error "luacretro: no coroutine context switch for this instruction set"
#endif

/* ---- state ------------------------------------------------------------------------ */

lv *lc_top;
lv *lc_stack_lo;
lv *lc_stack_hi;
char *lc_cstack_limit;
int lc_curline;

static lc_thr main_thr;
static lc_thr *cur;
static char *main_clo, *main_chi;
static lv *main_vlo, *main_vhi;

static uint8_t co_cregion[LC_CO_REGION] __attribute__((aligned(16)));
static lv co_vregion[LC_CO_VREGION];
static uint8_t sched_stack[LC_SCHED_STACK] __attribute__((aligned(16)));
#define REGION_TOP (co_cregion + sizeof co_cregion)
static void *sched_sp;
static lc_thr *sw_from, *sw_to;

static lv xfer[XFER_MAX];
static int nxfer;

static void load_cur(void) {
  lc_top = cur->vtop;
  if (cur == &main_thr) {
    lc_stack_lo = main_vlo;
    lc_stack_hi = main_vhi;
    lc_cstack_limit = main_clo + CSTACK_MARGIN;
  } else {
    lc_stack_lo = co_vregion;
    lc_stack_hi = co_vregion + LC_CO_VREGION - 8;
    lc_cstack_limit = (char *)co_cregion + CSTACK_MARGIN;
  }
}

void lc_co_init(char *cstack_base) {
  main_thr.status = CO_RUNNING;
  main_thr.started = 1;
  main_chi = cstack_base;
  cur = &main_thr;
}

static void to_xfer(const lv *v, int n) {
  if (n > XFER_MAX) lc_error("too many values to pass to/from a coroutine");
  for (int i = 0; i < n; i++) xfer[i] = v[i];
  nxfer = n;
}

/* ---- the copying trampoline --------------------------------------------------------- */

static void sched_loop(void) {
  for (;;) {
    lc_thr *f = sw_from, *t = sw_to;
    if (f != &main_thr && f->status != CO_DEAD) {
      uint32_t cs = (uint32_t)(REGION_TOP - (uint8_t *)f->csp);
      lc_memcpy(f->cbuf, f->csp, cs);
      f->csize = cs;
      uint32_t vs = (uint32_t)(f->vtop - co_vregion);
      lc_memcpy(f->vbuf, co_vregion, vs * sizeof(lv));
      f->vsize = vs;
    }
    if (t != &main_thr) {
      t->csp = REGION_TOP - t->csize;
      lc_memcpy(t->csp, t->cbuf, t->csize);
      lc_memcpy(co_vregion, t->vbuf, t->vsize * sizeof(lv));
    }
    lc_ctx_swap(&sched_sp, t->csp);
  }
}

static void *grow(uint8_t type, void *old, uint32_t *cap, uint32_t need, uint32_t unit) {
  (void)type;
  if (old && *cap >= need) return old;
  uint32_t n = need + 32;
  uint8_t *b = (uint8_t *)lc_alloc(LC_OVEC, 8 + (size_t)n * unit);
  *cap = n;
  return b + 8;
}

/* Switch from the running thread to another. `from` has already been
 * given its new status; `cur` must already point at `to`. */
static void switch_to(lc_thr *from, lc_thr *to) {
  if (from != &main_thr && from->status != CO_DEAD) {
    /* reserve the save buffers now, on from's own stack: the trampoline
     * must not allocate (a collection there would miss this stack) */
    char probe;
    uint32_t need = (uint32_t)(REGION_TOP - (uint8_t *)&probe) + 256;
    if (need > LC_CO_REGION) need = LC_CO_REGION;
    cur = from;                         /* collections below scan from's stacks */
    from->cbuf = (uint8_t *)grow(LC_OVEC, from->cbuf, &from->ccap, need, 1);
    uint32_t vneed = (uint32_t)(from->vtop - co_vregion);
    from->vbuf = (lv *)grow(LC_OVEC, from->vbuf, &from->vcap, vneed ? vneed : 1, sizeof(lv));
    cur = to;
  }
  sw_from = from;
  sw_to = to;
  lc_ctx_swap(&from->csp, sched_sp);
}

/* ---- boot / create ------------------------------------------------------------------- */

static void co_boot(void) {
  lc_thr *co = cur;
  load_cur();
  lv *b = co_vregion;
  int n = nxfer;
  for (int i = 0; i < n; i++) b[i] = xfer[i];
  lc_top = b + n;
  n = lc_call(co->fn, b, n);
  to_xfer(b, n);
  co->status = CO_DEAD;
  co->cbuf = 0; co->vbuf = 0; co->ccap = co->vcap = 0;
  lc_thr *back = co->resumer;
  co->resumer = 0;
  back->status = CO_RUNNING;
  cur = back;
  switch_to(co, back);
  for (;;) {}                    /* a dead coroutine is never switched to */
}

void *lc_ctx_prepare(uintptr_t top, void (*entry)(void)) {
  top &= ~(uintptr_t)15;
#if defined(__x86_64__)
  uintptr_t *sp = (uintptr_t *)(top - 16);
  sp[0] = (uintptr_t)entry;
  sp -= CTX_WORDS;
  for (int i = 0; i < CTX_WORDS; i++) sp[i] = 0;
#else
  uint32_t *sp = (uint32_t *)top;
  *--sp = (uint32_t)(uintptr_t)entry;
  for (int i = 0; i < CTX_WORDS; i++) *--sp = 0;
#endif
  return sp;
}

lv lc_co_create(lv fn) {
  if (LV_TAG(fn) != LT_FUN) lc_error("bad argument #1 to 'cocreate' (function expected)");
  lc_thr *co = (lc_thr *)lc_alloc(LC_OTHR, sizeof(lc_thr));
  lv *base = lc_top;
  base[0] = LV_OBJ(LT_THR, co);           /* keep co alive across the allocations */
  lc_top = base + 1;
  co->fn = fn;
  /* the initial frame is built as if it had been saved: the trampoline
   * copies it to the top of the region on the first resume */
  uint8_t frame[64 + 16 * 8];
  void *sp = lc_ctx_prepare((uintptr_t)(frame + sizeof frame), co_boot);
  uint32_t fsz = (uint32_t)((frame + sizeof frame) - (uint8_t *)sp);
  /* the frame is position independent (register images and a return
   * address), except that the region top must keep the same alignment */
  uint32_t pad = (uint32_t)((uintptr_t)(frame + sizeof frame) & 15);
  fsz -= pad;
  co->cbuf = (uint8_t *)grow(LC_OVEC, 0, &co->ccap, fsz + 64, 1);
  lc_memcpy(co->cbuf, sp, fsz);
  co->csize = fsz;
  co->vbuf = (lv *)grow(LC_OVEC, 0, &co->vcap, 8, sizeof(lv));
  co->vsize = 0;
  co->vtop = co_vregion;
  co->status = CO_SUSPENDED;
  lc_top = base;
  return LV_OBJ(LT_THR, co);
}

/* coresume(co, ...) -> true, yielded/returned values | false, error */
int lc_co_resume(lv *base, int nargs) {
  lv cov = nargs > 0 ? base[0] : LV_NIL;
  if (LV_TAG(cov) != LT_THR) lc_error("bad argument #1 to 'coresume' (coroutine expected)");
  lc_thr *co = (lc_thr *)LV_PTR(cov);
  if (co->status != CO_SUSPENDED) {
    base[0] = LV_FALSE;
    base[1] = LV_STR(lc_str_cstr(co->status == CO_DEAD ? "cannot resume dead coroutine"
                                                       : "cannot resume non-suspended coroutine"));
    return 2;
  }
  to_xfer(base + 1, nargs - 1);
  co->started = 1;
  lc_thr *me = cur;
  me->vtop = base + nargs;
  me->status = CO_NORMAL;
  co->resumer = me;
  co->status = CO_RUNNING;
  cur = co;
  switch_to(me, co);
  /* back here after a yield, return or error; cur == me */
  load_cur();
  lc_top = base;
  if (co->failed) {
    co->failed = 0;
    base[0] = LV_FALSE;
    base[1] = co->err;
    co->err = LV_NIL;
    return 2;
  }
  int nr = nxfer;
  base[0] = LV_TRUE;
  if (base + 1 + nr > lc_stack_hi) lc_error("stack overflow");
  for (int i = 0; i < nr; i++) base[1 + i] = xfer[i];
  return nr + 1;
}

int lc_co_yield(lv *base, int nargs) {
  lc_thr *co = cur;
  if (co == &main_thr) lc_error("attempt to yield from outside a coroutine");
  to_xfer(base, nargs);
  co->vtop = base;
  co->status = CO_SUSPENDED;
  lc_thr *back = co->resumer;
  co->resumer = 0;
  back->status = CO_RUNNING;
  cur = back;
  switch_to(co, back);
  /* resumed: cur == co again */
  load_cur();
  int n = nxfer;
  if (base + n > lc_stack_hi) lc_error("stack overflow");
  for (int i = 0; i < n; i++) base[i] = xfer[i];
  lc_top = base + n;
  return n;
}

lv lc_co_status(lv v) {
  if (LV_TAG(v) != LT_THR) lc_error("bad argument #1 to 'costatus' (coroutine expected)");
  static const char *const names[] = { "suspended", "running", "normal", "dead" };
  return LV_STR(lc_str_cstr(names[((lc_thr *)LV_PTR(v))->status]));
}

int lc_co_running(void) { return cur != &main_thr; }

_Noreturn void lc_error(const char *msg) {
  lc_thr *co = cur;
  if (co == &main_thr) {
    if (lc_curline) {
      static char buf[200];
      int n = 0, l = lc_curline;
      const char *p = "line ";
      while (*p) buf[n++] = *p++;
      char d[8]; int k = 0;
      do { d[k++] = (char)('0' + l % 10); l /= 10; } while (l);
      while (k) buf[n++] = d[--k];
      buf[n++] = ':'; buf[n++] = ' ';
      while (*msg && n < 198) buf[n++] = *msg++;
      buf[n] = 0;
      lc_plat_fatal(buf);
    }
    lc_plat_fatal(msg);
  }
  /* the message may live in a static buffer that the next error reuses */
  co->err = LV_STR(lc_str_cstr(msg));
  co->failed = 1;
  co->status = CO_DEAD;
  co->cbuf = 0; co->vbuf = 0; co->ccap = co->vcap = 0;
  lc_thr *back = co->resumer;
  co->resumer = 0;
  back->status = CO_RUNNING;
  cur = back;
  switch_to(co, back);
  for (;;) {}
}

/* ---- gc ------------------------------------------------------------------------------ */

static void mark_buffers(lc_thr *t) {
  if (t->cbuf) {
    lc_gc_mark_ptr(t->cbuf - 8);
    if (t != cur) lc_gc_scan_range(t->cbuf, t->cbuf + t->csize);
  }
  if (t->vbuf) {
    lc_gc_mark_ptr((uint8_t *)t->vbuf - 8);
    if (t != cur) lc_gc_scan_range(t->vbuf, t->vbuf + t->vsize);
  }
}

void lc_co_traverse(lc_hdr *h) {
  lc_thr *t = (lc_thr *)h;
  lc_gc_mark_value(t->fn);
  lc_gc_mark_value(t->err);
  mark_buffers(t);
}

void lc_co_mark_stacks(const void *sp_now) {
  /* the running thread's live stacks */
  if (cur == &main_thr) {
    lc_gc_scan_range(main_vlo, lc_top);
    lc_gc_scan_range(sp_now, main_chi);
  } else {
    lc_gc_scan_range(co_vregion, lc_top);
    lc_gc_scan_range(sp_now, REGION_TOP);
    lc_gc_mark_ptr(cur);
  }
  /* threads waiting in a resume call: coroutines are saved, main is in place */
  for (lc_thr *t = cur->resumer; t; t = t->resumer) {
    if (t == &main_thr) {
      lc_gc_scan_range(main_vlo, t->vtop);
      lc_gc_scan_range(t->csp, main_chi);
    } else lc_gc_mark_ptr(t);
  }
  lc_gc_scan_range(xfer, xfer + nxfer);
}

/* ---- init ---------------------------------------------------------------------------- */

void lc_tm_init(void);
void lc_lib_mark_roots(void);
void lc_str_mark_table(void);

void lc_rt_mark_roots(void) {
  lc_str_mark_table();
  for (int i = 0; i < TM_N; i++) if (lc_tmname[i]) lc_gc_mark_ptr(lc_tmname[i]);
  if (lc_string_meta) lc_gc_mark_ptr(lc_string_meta);
  lc_lib_mark_roots();
}

void lc_init_rt(void *arena, size_t arena_size, lv *vstack, size_t vslots, char *cstack_base, size_t cstack_size) {
  lc_gc_init(arena, arena_size);
  main_vlo = vstack;
  main_vhi = vstack + vslots - 8;
  main_thr.vtop = vstack;
  main_thr.resumer = 0;
  main_clo = cstack_base - cstack_size;
  lc_co_init(cstack_base);
  load_cur();
  nxfer = 0;
  sched_sp = lc_ctx_prepare((uintptr_t)(sched_stack + sizeof sched_stack), sched_loop);
  lc_string_meta = 0;
  lc_str_init();
}

/* After the cart has interned its literals: the runtime's own names must
 * not be created first, or a literal and the runtime would hold two
 * distinct objects for the same text (keys compare by identity). */
void lc_init_rt_names(void) {
  lc_tm_init();
}
