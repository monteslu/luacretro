/* luacretro runtime: PICO-8 system - memory, input, RNG, time, cartdata and
 * the frame loop.
 *
 * The loop reproduces the PICO-8 player's main coroutine (the zepto8 BIOS
 * used by FAKE-08): _init once, then per frame _update60 (or, at 30 fps, an
 * idle tick followed by _update) and _draw, with one 60 Hz tick per frame
 * handed to the platform. run() restarts the cart from a fresh C stack
 * instead of unwinding, so it never grows the stack.
 */
#include "lc.h"
#include "lc_int.h"
#include "lc_p8.h"
#include "lc_plat.h"
#include "lc_cart.h"

LC_BIGDATA uint8_t lc_p8mem[LC_P8_MEMSIZE];
const uint8_t *lc_p8_cartrom;
uint32_t lc_p8_frames;
uint8_t lc_p8_fps = 30;

/* ---- memory ---------------------------------------------------------------------- */

void lc_p8_reset_memory(void) {
  lc_memset(lc_p8mem, 0, 0x4300);
  lc_memset(lc_p8mem + 0x5f00, 0, (LC_P8_MEMSIZE < 0x8000 ? LC_P8_MEMSIZE : 0x8000) - 0x5f00);
  lc_p8mem[P8_COLORMASK] = 0xff;
  lc_p8mem[P8_SPRMAP] = 0x00;
  lc_p8mem[P8_SCRMAP] = 0x60;
  lc_p8mem[P8_MAPMAP] = 0x20;
  lc_p8mem[P8_MAPW] = 128;
  /* reload() */
  for (uint32_t i = 0; i < 0x4300; i++) lc_p8mem[i] = lc_p8_cartrom[i];
}

void lc_p8_reload(int dest, int src, int len) {
  if (len <= 0) return;
  if (dest < 0 || dest > 0x10000) return;
  if (src < 0 || src > 0x8000) return;
  if (dest + len > 0x10000) return;
  for (int i = 0; i < len; i++) {
    int s = src + i;
    p8_wr8((uint32_t)(dest + i), s < 0x4300 ? lc_p8_cartrom[s] : 0);
  }
}

void lc_p8_memcpy(int dest, int src, int len) {
  if (len <= 0) return;
  if (src < 0 || src + len > 0x10000) return;
  if (dest < 0 || dest + len > 0x10000) return;
  if (dest <= src) for (int i = 0; i < len; i++) p8_wr8((uint32_t)(dest + i), p8_rd8((uint32_t)(src + i)));
  else for (int i = len - 1; i >= 0; i--) p8_wr8((uint32_t)(dest + i), p8_rd8((uint32_t)(src + i)));
}

void lc_p8_memset(int dest, int val, int len) {
  if (len <= 0) return;
  if (dest < 0 || dest + len > 0x10000) return;
  for (int i = 0; i < len; i++) p8_wr8((uint32_t)(dest + i), (uint8_t)val);
}

/* peek2/peek4 wrap past the top of memory (zepto8) */
int16_t lc_p8_peek2(uint32_t a) {
  return (int16_t)(p8_rd8(a & 0xffff) | (p8_rd8((a + 1) & 0xffff) << 8));
}
int32_t lc_p8_peek4(uint32_t a) {
  uint32_t b = 0;
  for (int i = 0; i < 4; i++) b |= (uint32_t)p8_rd8((a + (uint32_t)i) & 0xffff) << (8 * i);
  return (int32_t)b;
}
void lc_p8_poke2(uint32_t a, int16_t v) {
  if (a > 0xffff - 1) return;
  p8_wr8(a, (uint8_t)v);
  p8_wr8(a + 1, (uint8_t)((uint16_t)v >> 8));
}
void lc_p8_poke4(uint32_t a, int32_t v) {
  if (a > 0xffff - 3) return;
  p8_wr32(a, (uint32_t)v);
}

/* ---- cartdata -------------------------------------------------------------------- */

static char cd_key[65];
static int cd_dirty;
static uint32_t cd_last_save;

int lc_p8_cartdata(const char *key, uint32_t len) {
  if (len == 0 || len > 64) lc_error("cart data id too long");
  if (cd_key[0] && cd_dirty) lc_plat_cartdata_save(cd_key, lc_p8mem + P8_CARTDATA);
  int same = 1;
  for (uint32_t i = 0; i < len; i++) if (cd_key[i] != key[i]) same = 0;
  if (cd_key[len] != 0) same = 0;
  if (!same) for (int i = 0; i < 256; i++) lc_p8mem[P8_CARTDATA + i] = 0;
  for (uint32_t i = 0; i < len; i++) cd_key[i] = key[i];
  cd_key[len] = 0;
  cd_dirty = 0;
  return lc_plat_cartdata_load(cd_key, lc_p8mem + P8_CARTDATA);
}

void lc_p8_cartdata_touch(void) { if (cd_key[0]) cd_dirty = 1; }

static void cartdata_flush(void) {
  if (cd_dirty && lc_p8_frames - cd_last_save >= 60) {
    lc_plat_cartdata_save(cd_key, lc_p8mem + P8_CARTDATA);
    cd_dirty = 0;
    cd_last_save = lc_p8_frames;
  }
}

/* ---- input ------------------------------------------------------------------------- */

static uint8_t held[8], down[8], prevheld[8];
static uint16_t frames_held[8][8];

void lc_p8_update_buttons(void) {
  uint8_t delay = lc_p8mem[P8_BTNP_DELAY] ? lc_p8mem[P8_BTNP_DELAY] : 15;
  uint8_t rate = lc_p8mem[P8_BTNP_RATE] ? lc_p8mem[P8_BTNP_RATE] : 4;
  for (int p = 0; p < 8; p++) {
    uint8_t h = lc_plat_buttons(p);
    uint8_t d = (uint8_t)(h & ~prevheld[p]);
    prevheld[p] = h;
    /* the pause button only fires on its first frame */
    if ((h & 0x40) && !(d & 0x40)) h &= (uint8_t)~0x40;
    held[p] = h;
    if (p < 8) lc_p8mem[P8_BTNSTATE + p] = h & 0x3f;
    if (delay != 255) {
      for (int i = 0; i < 7; i++) {
        int isdown = (h >> i) & 1;
        frames_held[p][i] = isdown ? (uint16_t)(frames_held[p][i] + 1) : 0;
        uint16_t f = frames_held[p][i];
        if (f == delay || (f / delay >= 1 && f % rate == 0)) d |= (uint8_t)(1 << i);
      }
    }
    down[p] = d;
  }
}

int lc_p8_btn(int i, int p) {
  if (i < 0) return held[0] | (held[1] << 8);
  if (p < 0 || p > 7 || i > 7) return 0;
  return (held[p] >> i) & 1;
}

int lc_p8_btnp(int i, int p) {
  if (i < 0) return down[0] | (down[1] << 8);
  if (p < 0 || p > 7 || i > 7) return 0;
  return (down[p] >> i) & 1;
}

/* ---- rng (PICO-8's generator, state at 0x5f44) -------------------------------------- */

static void prng_step(void) {
  uint32_t s0 = p8_rd32(P8_RNG), s1 = p8_rd32(P8_RNG + 4);
  s1 = s0 + ((s1 >> 16) | (s1 << 16));
  s0 += s1;
  p8_wr32(P8_RNG, s0);
  p8_wr32(P8_RNG + 4, s1);
}

int32_t lc_p8_rnd(int32_t range) {
  prng_step();
#ifdef LC_TRACE_RND
  { extern void lc_host_trace_rnd(uint32_t f, int32_t r); lc_host_trace_rnd(lc_p8_frames, range); }
#endif
  uint32_t b = p8_rd32(P8_RNG + 4);
  uint32_t r = (uint32_t)range;
  /* the range is taken as unsigned bits: rnd(-x) draws from [0, 2^32 - x) */
  return (int32_t)(r > 0 ? b % r : 0);
}

void lc_p8_srand(int32_t seed) {
  uint32_t s0 = seed ? (uint32_t)seed : 0xdeadbeefu;
  p8_wr32(P8_RNG, s0);
  p8_wr32(P8_RNG + 4, s0 ^ 0xbead29bau);
  for (int i = 0; i < 32; i++) prng_step();
}

/* ---- frame loop ---------------------------------------------------------------------- */

static void tick_present(void) {
  lc_plat_present(lc_p8_screenbuf(), lc_p8mem + P8_SCREENPAL, lc_p8mem[P8_DRAWMODE]);
  lc_p8_frames++;
  cartdata_flush();
}

static void tick_idle(void) {
  lc_plat_vsync();
  lc_p8_frames++;
}

void lc_p8_flip(void) {
  tick_present();
  if (lc_p8_fps == 30) tick_idle();
  lc_p8_update_buttons();
}

static void call0(lv f) {
  lv *b = lc_top;
  lc_call(f, b, 0);
  lc_top = b;
}

static void *rt_arena;
static size_t rt_arena_size;
static lv *rt_vstack;
static size_t rt_vslots;
static char *rt_cbase;
static size_t rt_csize;

volatile int lc_boot_stage;

static _Noreturn void cart_boot(void) {
  lc_boot_stage = 1;
  lc_init_rt(rt_arena, rt_arena_size, rt_vstack, rt_vslots, rt_cbase, rt_csize);
  lc_boot_stage = 2;
  lc_p8_cartrom = lc_cart_rom;
  lc_p8_reset_memory();
  lc_p8_reset_drawstate();
  lc_p8_audio_init();
  lc_p8_srand((int32_t)lc_plat_seed());
  for (int p = 0; p < 8; p++) { held[p] = down[p] = prevheld[p] = 0; for (int i = 0; i < 8; i++) frames_held[p][i] = 0; }
  lc_p8_frames = 1;
  lc_boot_stage = 3;
  lc_cart_setup();
  lc_boot_stage = 4;
  lc_init_rt_names();
  lc_boot_stage = 5;
  lc_cart_main();
  lc_boot_stage = 6;
  /* PICO-8 picks the frame rate once the program has defined its callbacks */
  lc_p8_fps = lc_cart_cb(LC_CB_UPDATE60) != LV_NIL ? 60 : 30;
  lv init = lc_cart_cb(LC_CB_INIT);
  if (init != LV_NIL) call0(init);
  if (lc_cart_cb(LC_CB_UPDATE) == LV_NIL && lc_cart_cb(LC_CB_UPDATE60) == LV_NIL && lc_cart_cb(LC_CB_DRAW) == LV_NIL) {
    /* the program ended: keep showing its last frame */
    for (;;) tick_present();
  }
  for (;;) {
    lv u60 = lc_cart_cb(LC_CB_UPDATE60);
    if (u60 != LV_NIL) {
      lc_p8_fps = 60;
      lc_p8_update_buttons();
      call0(u60);
    } else {
      lc_p8_fps = 30;
      tick_idle();
      lc_p8_update_buttons();
      lv u = lc_cart_cb(LC_CB_UPDATE);
      if (u != LV_NIL) call0(u);
    }
    lv d = lc_cart_cb(LC_CB_DRAW);
    if (d != LV_NIL) call0(d);
    tick_present();
  }
}

void lc_ctx_swap(void **save_sp, void *new_sp);

/* Start (or restart) the cart on a fresh frame at the top of the main C
 * stack. Whatever was running is abandoned, which is exactly run(). The
 * new frame is built from a small separate boot stack, because the code
 * doing the switch may itself be running in the region being rebuilt. */
static uint8_t boot_stack[256] __attribute__((aligned(16)));
static void *boot_sp, *dead_sp;

static _Noreturn void boot_trampoline(void) {
  void *sp = lc_ctx_prepare(((uintptr_t)rt_cbase - 64) & ~(uintptr_t)15, cart_boot);
  lc_ctx_swap(&boot_sp, sp);
  for (;;) {}
}

static _Noreturn void start_fresh(void) {
  void *sp = lc_ctx_prepare((uintptr_t)(boot_stack + sizeof boot_stack), boot_trampoline);
  lc_ctx_swap(&dead_sp, sp);
  for (;;) {}
}

_Noreturn void lc_p8_run(void *arena, size_t arena_size, void *vstack, size_t vslots, char *cstack_base, size_t cstack_size) {
  rt_arena = arena;
  rt_arena_size = arena_size;
  rt_vstack = (lv *)vstack;
  rt_vslots = vslots;
  rt_cbase = cstack_base;
  rt_csize = cstack_size;
  start_fresh();
}

_Noreturn void lc_p8_restart(void) {
  lc_p8_audio_init();
  start_fresh();
}
