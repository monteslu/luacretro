/* luacretro runtime: internal interfaces shared between runtime modules. */
#ifndef LC_INT_H
#define LC_INT_H
#include "lc.h"

void lc_gc_init(void *arena, size_t size);
int lc_in_arena(const void *p);
void lc_gc_mark_value(lv v);
void lc_gc_mark_ptr(void *p);
void lc_gc_scan_range(const void *lo, const void *hi);
int lc_gc_is_marked(const void *p);
void lc_gc_lock(int delta);

void lc_str_init(void);
void lc_str_sweep_intern(void);
void lc_rt_mark_roots(void);
void lc_co_init(char *cstack_base);
void lc_co_traverse(lc_hdr *thr);
void lc_co_mark_stacks(const void *sp_now);
void *lc_ctx_prepare(uintptr_t top, void (*entry)(void));
void lc_ctx_swap(void **save_sp, void *new_sp);
void lc_init_rt_names(void);
void lc_init_rt(void *arena, size_t arena_size, lv *vstack, size_t vslots, char *cstack_base, size_t cstack_size);

void *lc_memcpy(void *d, const void *s, size_t n);
void *lc_memset(void *d, int c, size_t n);
int lc_memcmp(const void *a, const void *b, size_t n);
size_t lc_strlen(const char *s);

/* metamethod name strings (interned at init) */
enum { TM_INDEX, TM_NEWINDEX, TM_CALL, TM_ADD, TM_SUB, TM_MUL, TM_DIV, TM_MOD,
       TM_POW, TM_IDIV, TM_UNM, TM_CONCAT, TM_LEN, TM_EQ, TM_LT, TM_LE,
       TM_BAND, TM_BOR, TM_BXOR, TM_SHL, TM_SHR, TM_LSHR, TM_ROTL, TM_ROTR,
       TM_BNOT, TM_TOSTRING, TM_METATABLE, TM_PAIRS, TM_IPAIRS, TM_N };
extern lc_str *lc_tmname[TM_N];
lv lc_gettm(lv o, int ev);

extern lc_tab *lc_string_meta;    /* shared metatable for strings, may be NULL */

#endif
