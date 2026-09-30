/* luacretro runtime: the PICO-8 machine (memory map, draw state, gfx, text,
 * audio, input) shared by every dynamic-tier target.
 *
 * All state lives in lc_p8mem, laid out exactly like PICO-8's address space
 * so peek/poke/memcpy see what carts expect. Multi-byte fields are always
 * little-endian byte accesses (never struct overlays), so big-endian
 * targets behave identically.
 *
 * Reference: the behaviour follows FAKE-08 (MIT, jtothebell), itself built
 * on zepto8 (WTFPL, Sam Hocevar); both are the oracle the conformance
 * tests diff against.
 */
#ifndef LC_P8_H
#define LC_P8_H
#include <stdint.h>
#include <stddef.h>

#ifndef LC_P8_MEMSIZE
#define LC_P8_MEMSIZE 0x10000u   /* 0x8000 on targets without upper RAM */
#endif

extern uint8_t lc_p8mem[LC_P8_MEMSIZE];
extern const uint8_t *lc_p8_cartrom;      /* 0x4300 bytes of cart data (gfx..sfx) */

/* ---- memory map ------------------------------------------------------------------- */
#define P8_GFX        0x0000
#define P8_MAP        0x2000
#define P8_GFF        0x3000
#define P8_MUSIC      0x3100
#define P8_SFX        0x3200
#define P8_CARTDATA   0x5e00
#define P8_DRAWPAL    0x5f00   /* [16] draw palette: low nibble color, bit 4 = transparent */
#define P8_SCREENPAL  0x5f10   /* [16] screen palette */
#define P8_CLIP_XB    0x5f20
#define P8_CLIP_YB    0x5f21
#define P8_CLIP_XE    0x5f22
#define P8_CLIP_YE    0x5f23
#define P8_COLOR      0x5f25
#define P8_TEXT_X     0x5f26
#define P8_TEXT_Y     0x5f27
#define P8_CAMERA_X   0x5f28   /* int16 */
#define P8_CAMERA_Y   0x5f2a   /* int16 */
#define P8_DRAWMODE   0x5f2c
#define P8_DEVKIT     0x5f2d
#define P8_PERSISTPAL 0x5f2e
#define P8_SNDPAUSE   0x5f2f
#define P8_NOPAUSE    0x5f30
#define P8_FILLP      0x5f31   /* [2] */
#define P8_FILLP_T    0x5f33
#define P8_COLORFLAG  0x5f34
#define P8_LINEINV    0x5f35
#define P8_MISCFLAGS  0x5f36
#define P8_TLINE_W    0x5f38
#define P8_TLINE_H    0x5f39
#define P8_TLINE_X    0x5f3a
#define P8_TLINE_Y    0x5f3b
#define P8_LINE_X     0x5f3c   /* int16 */
#define P8_LINE_Y     0x5f3e   /* int16 */
#define P8_HALFRATE   0x5f40
#define P8_REVERB     0x5f41
#define P8_DISTORT    0x5f42
#define P8_LOWPASS    0x5f43
#define P8_RNG        0x5f44   /* uint32[2] */
#define P8_BTNSTATE   0x5f4c   /* [8] */
#define P8_SPRMAP     0x5f54
#define P8_SCRMAP     0x5f55
#define P8_MAPMAP     0x5f56
#define P8_MAPW       0x5f57
#define P8_PRINTATTR  0x5f58
#define P8_PRINTDIM   0x5f59
#define P8_PRINTTAB   0x5f5a
#define P8_PRINTOFF   0x5f5b
#define P8_BTNP_DELAY 0x5f5c
#define P8_BTNP_RATE  0x5f5d
#define P8_COLORMASK  0x5f5e
#define P8_ALTPALFLAG 0x5f5f
#define P8_ALTPAL     0x5f60   /* [16] */
#define P8_ALTPALROWS 0x5f70   /* [16] */
#define P8_GPIO       0x5f80
#define P8_SCREEN     0x6000

static inline uint8_t p8_rd8(uint32_t a) { return a < LC_P8_MEMSIZE ? lc_p8mem[a] : 0; }
static inline void p8_wr8(uint32_t a, uint8_t v) { if (a < LC_P8_MEMSIZE) lc_p8mem[a] = v; }
static inline int16_t p8_rd16(uint32_t a) { return (int16_t)(p8_rd8(a) | (p8_rd8(a + 1) << 8)); }
static inline void p8_wr16(uint32_t a, int16_t v) { p8_wr8(a, (uint8_t)v); p8_wr8(a + 1, (uint8_t)((uint16_t)v >> 8)); }
static inline uint32_t p8_rd32(uint32_t a) {
  return (uint32_t)p8_rd8(a) | ((uint32_t)p8_rd8(a + 1) << 8) | ((uint32_t)p8_rd8(a + 2) << 16) | ((uint32_t)p8_rd8(a + 3) << 24);
}
static inline void p8_wr32(uint32_t a, uint32_t v) {
  p8_wr8(a, (uint8_t)v); p8_wr8(a + 1, (uint8_t)(v >> 8)); p8_wr8(a + 2, (uint8_t)(v >> 16)); p8_wr8(a + 3, (uint8_t)(v >> 24));
}

/* The buffers that the 0x5f54/0x5f55 remapping selects. */
uint8_t *lc_p8_screenbuf(void);
uint8_t *lc_p8_spritebuf(void);

/* ---- lifecycle --------------------------------------------------------------------- */
void lc_p8_reset_memory(void);            /* PicoRam::Reset + cart reload */
void lc_p8_reset_drawstate(void);         /* clip() camera() pal() color() fillp() */

/* ---- graphics (lc_p8gfx.c) --------------------------------------------------------- */
void lc_p8_cls(int col);
void lc_p8_pset(int x, int y, int col);         /* col < 0: current pen */
int  lc_p8_pget(int x, int y);
void lc_p8_line(int x0, int y0, int x1, int y1, int col);
void lc_p8_line_to(int x1, int y1, int col);    /* continue from last line end */
void lc_p8_line_invalidate(void);
void lc_p8_rect(int x0, int y0, int x1, int y1, int col);
void lc_p8_rectfill(int x0, int y0, int x1, int y1, int col);
void lc_p8_rrect(int x, int y, int w, int h, int r, int col);
void lc_p8_rrectfill(int x, int y, int w, int h, int r, int col);
void lc_p8_circ(int x, int y, int r, int col);
void lc_p8_circfill(int x, int y, int r, int col);
void lc_p8_oval(int x0, int y0, int x1, int y1, int col);
void lc_p8_ovalfill(int x0, int y0, int x1, int y1, int col);
void lc_p8_spr(int n, int x, int y, int32_t w, int32_t h, int flipx, int flipy);  /* w,h 16.16 */
void lc_p8_sspr(int sx, int sy, int sw, int sh, int dx, int dy, int dw, int dh, int flipx, int flipy);
void lc_p8_map(int cx, int cy, int sx, int sy, int cw, int ch, int layer);
void lc_p8_tline(int x0, int y0, int x1, int y1, int32_t mx, int32_t my, int32_t mdx, int32_t mdy, int layers);
int  lc_p8_mget(int x, int y);
void lc_p8_mset(int x, int y, int v);
int  lc_p8_sget(int x, int y);
void lc_p8_sset(int x, int y, int c);
int  lc_p8_fget(int n);
void lc_p8_fset(int n, int v);
void lc_p8_pal_reset(int which);                /* pal() / pal(p) */
int  lc_p8_pal(int c0, int c1, int p);          /* returns previous */
void lc_p8_palt_reset(void);
int  lc_p8_palt(int c, int t);
int32_t lc_p8_fillp(int32_t pat);               /* returns previous (16.16) */
int  lc_p8_color(int c);                        /* returns previous */
void lc_p8_camera(int x, int y, int *px, int *py);
void lc_p8_clip(int x, int y, int w, int h, int clip_prev, int reset, int *prev4);
void lc_p8_cursor(int x, int y, int col, int *px, int *py, int *pcol);
/* set the pen colour through PICO-8's colour/fill-pattern rules */
void lc_p8_set_pen(int col);

/* per-pixel primitive shared with the text renderer */
void lc_p8_plot_pen(int x, int y);              /* camera already applied, clip checked */
void lc_p8_plot_sprite(int x, int y, int c);    /* draw-palette mapped, bitmask applied */

/* ---- text (lc_p8print.c) ------------------------------------------------------------ */
/* print(str, [x, y,] [col]); has_xy / has_col say which were given.
 * Returns the x coordinate after the last character (PICO-8's return). */
int lc_p8_print(const uint8_t *s, uint32_t len, int has_xy, int x, int y, int has_col, int col);
extern const uint8_t lc_p8_font[];            /* built-in font, 8 bytes per glyph */

/* ---- audio (lc_p8snd.c) -------------------------------------------------------------- */
void lc_p8_audio_init(void);
void lc_p8_sfx(int n, int channel, int offset, int length);
void lc_p8_music(int n, int fade_ms, int mask);
int  lc_p8_audio_stat(int n);                   /* stat(16..26, 46..57) */
/* Synthesize n samples of 22050 Hz mono s16 into out. */
void lc_p8_audio_fill(int16_t *out, int n);

/* ---- input (lc_p8sys.c) ---------------------------------------------------------------- */
void lc_p8_update_buttons(void);
int  lc_p8_btn(int i, int p);                   /* i < 0: bitfield */
int  lc_p8_btnp(int i, int p);

/* ---- system (lc_p8sys.c) --------------------------------------------------------------- */
extern uint32_t lc_p8_frames;                   /* ticks since cart start */
extern uint8_t lc_p8_fps;                       /* 30 or 60 */
int32_t lc_p8_rnd(int32_t range);               /* range 16.16 */
void lc_p8_srand(int32_t seed);
void lc_p8_flip(void);
void lc_p8_reload(int dest, int src, int len);
void lc_p8_memcpy(int dest, int src, int len);
void lc_p8_memset(int dest, int val, int len);
int16_t lc_p8_peek2(uint32_t a);
int32_t lc_p8_peek4(uint32_t a);
void lc_p8_poke2(uint32_t a, int16_t v);
void lc_p8_poke4(uint32_t a, int32_t v);
int lc_p8_cartdata(const char *key, uint32_t len);
void lc_p8_cartdata_touch(void);
/* Entry point: never returns. The C stack region [cstack_base - cstack_size,
 * cstack_base) must be the stack the caller is running on. */
_Noreturn void lc_p8_run(void *arena, size_t arena_size, void *vstack, size_t vslots, char *cstack_base, size_t cstack_size);
_Noreturn void lc_p8_restart(void);

#endif
