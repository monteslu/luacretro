/* luacretro runtime: the PICO-8 API as Lua-callable builtins.
 *
 * Argument handling mirrors FAKE-08's picoluaapi.cpp: the number of
 * arguments actually passed selects the overload, missing or non-numeric
 * arguments read as 0 (lua_tonumber), and fixed-point values become
 * integers by dropping the fraction (bits >> 16, i.e. flooring).
 */
#include "lc.h"
#include "lc_int.h"
#include "lc_lib.h"
#include "lc_p8.h"
#include "lc_plat.h"

#define NUM(i) lc_argnum(B, N, (i))
#define INT(i) (lc_argnum(B, N, (i)) >> 16)
#define I16(i) ((int16_t)(lc_argnum(B, N, (i)) >> 16))
#define U16(i) ((uint16_t)(int16_t)(lc_argnum(B, N, (i)) >> 16))
#define BOOL(i) ((i) < N && LV_TRUTHY(B[(i)]))
#define RET_NUM(bits) do { B[0] = LV_NUM(bits); return 1; } while (0)
#define RET_INT(v) do { B[0] = LV_INT(v); return 1; } while (0)

/* ---- graphics ------------------------------------------------------------------------ */

LCB(cls) { lc_p8_cls(N == 0 ? 0 : INT(0)); return 0; }

LCB(pset) {
  int x = INT(0), y = INT(1);
  lc_p8_pset(x, y, N <= 2 ? -1 : (INT(2) & 0xff));
  return 0;
}

LCB(pget) { RET_INT(lc_p8_pget(INT(0), INT(1))); }

LCB(color) {
  int prev = N > 0 ? lc_p8_color((uint8_t)I16(0)) : lc_p8_color(6);
  RET_INT(prev);
}

LCB(line) {
  switch (N) {
    case 0: lc_p8_line_invalidate(); break;
    case 1: lc_p8_color(INT(0) & 0xff); lc_p8_line_invalidate(); break;
    case 2: lc_p8_line_to(INT(0), INT(1), lc_p8mem[P8_COLOR]); break;
    case 3: lc_p8_line_to(INT(0), INT(1), (uint8_t)I16(2)); break;
    case 4: lc_p8_line(INT(0), INT(1), INT(2), INT(3), lc_p8mem[P8_COLOR]); break;
    default: lc_p8_line(INT(0), INT(1), INT(2), INT(3), (uint8_t)I16(4)); break;
  }
  return 0;
}

LCB(tline) {
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  int32_t mx = 0, my = 0, mdx = 0x2000, mdy = 0;
  if (N >= 6) { x0 = INT(0); y0 = INT(1); x1 = INT(2); y1 = INT(3); mx = NUM(4); my = NUM(5); }
  if (N > 7) { mdx = NUM(6); mdy = NUM(7); }
  lc_p8_tline(x0, y0, x1, y1, mx, my, mdx, mdy, 0);
  return 0;
}

LCB(circ) {
  int ox = INT(0), oy = INT(1);
  if (N == 2) lc_p8_circ(ox, oy, 4, lc_p8mem[P8_COLOR]);
  else if (N == 3) lc_p8_circ(ox, oy, INT(2), lc_p8mem[P8_COLOR]);
  else if (N > 3) lc_p8_circ(ox, oy, INT(2), (uint8_t)I16(3));
  return 0;
}

LCB(circfill) {
  int ox = INT(0), oy = INT(1);
  if (N == 2) lc_p8_circfill(ox, oy, 4, lc_p8mem[P8_COLOR]);
  else if (N == 3) lc_p8_circfill(ox, oy, INT(2), lc_p8mem[P8_COLOR]);
  else if (N > 3) lc_p8_circfill(ox, oy, INT(2), (uint8_t)I16(3));
  return 0;
}

#define RECTLIKE(name, fn) \
  LCB(name) { \
    if (N >= 4) fn(INT(0), INT(1), INT(2), INT(3), N == 4 ? lc_p8mem[P8_COLOR] : (uint8_t)I16(4)); \
    return 0; \
  }
RECTLIKE(oval, lc_p8_oval)
RECTLIKE(ovalfill, lc_p8_ovalfill)
RECTLIKE(rect, lc_p8_rect)
RECTLIKE(rectfill, lc_p8_rectfill)

LCB(rrect) {
  if (N >= 5) lc_p8_rrect(INT(0), INT(1), INT(2), INT(3), INT(4), N == 5 ? lc_p8mem[P8_COLOR] : (uint8_t)I16(5));
  return 0;
}
LCB(rrectfill) {
  if (N >= 5) lc_p8_rrectfill(INT(0), INT(1), INT(2), INT(3), INT(4), N == 5 ? lc_p8mem[P8_COLOR] : (uint8_t)I16(5));
  return 0;
}

LCB(print) {
  if (N == 0) return 0;
  char nbuf[16];
  const char *s = "";
  uint32_t len = 0;
  lv v = B[0];
  switch (LV_TAG(v)) {
    case LT_NIL: s = "[nil]"; len = 5; break;
    case LT_STR: s = ((lc_str *)LV_PTR(v))->data; len = ((lc_str *)LV_PTR(v))->len; break;
    case LT_NUM: len = (uint32_t)lc_fmtnum(nbuf, LV_N(v)); s = nbuf; break;
    case LT_TRUE: s = "true"; len = 4; break;
    case LT_FALSE: s = "false"; len = 5; break;
    case LT_FUN: s = "[function]"; len = 10; break;
    default: break;
  }
  int nx;
  if (N < 2) nx = lc_p8_print((const uint8_t *)s, len, 0, 0, 0, 0, 0);
  else if (N == 2) { lc_p8_color((uint8_t)I16(1)); nx = lc_p8_print((const uint8_t *)s, len, 0, 0, 0, 0, 0); }
  else if (N == 3) nx = lc_p8_print((const uint8_t *)s, len, 1, INT(1), INT(2), 0, 0);
  else nx = lc_p8_print((const uint8_t *)s, len, 1, INT(1), INT(2), 1, (uint8_t)I16(3));
  B[0] = LV_INT(nx);
  return 1;
}

LCB(cursor) {
  int px, py, pc;
  lc_p8_cursor(INT(0), INT(1), N < 3 ? -1 : (uint8_t)I16(2), &px, &py, &pc);
  B[0] = LV_INT(px);
  B[1] = LV_INT(py);
  return 2;
}

LCB(spr) {
  if (N < 3) return 0;
  int32_t w = 0x10000, h = 0x10000;
  if (N > 3) w = NUM(3);
  if (N > 4) h = NUM(4);
  lc_p8_spr(INT(0), INT(1), INT(2), w, h, N > 5 && BOOL(5), N > 6 && BOOL(6));
  return 0;
}

LCB(sspr) {
  if (N < 6) return 0;
  int sw = INT(2), sh = INT(3);
  int dw = N > 6 ? INT(6) : sw, dh = N > 7 ? INT(7) : sh;
  lc_p8_sspr(INT(0), INT(1), sw, sh, INT(4), INT(5), dw, dh, N > 8 && BOOL(8), N > 9 && BOOL(9));
  return 0;
}

LCB(fget) {
  int n = INT(0) & 0xff;
  if (N == 1) RET_INT(lc_p8_fget(n));
  int f = INT(1) & 0xff;
  B[0] = LV_BOOL((lc_p8_fget(n) >> f) & 1);
  return 1;
}

LCB(fset) {
  int n = INT(0) & 0xff;
  if (N > 2) {
    int f = INT(1) & 0xff;
    int v = BOOL(2);
    int cur = lc_p8_fget(n);
    lc_p8_fset(n, v ? (cur | (1 << f)) : (cur & ~(1 << f)));
  } else lc_p8_fset(n, INT(1) & 0xff);
  return 0;
}

LCB(sget) { RET_INT(lc_p8_sget(INT(0), INT(1))); }
LCB(sset) { lc_p8_sset(INT(0), INT(1), (uint8_t)I16(2)); return 0; }

LCB(camera) {
  int16_t x = 0, y = 0;
  if (N > 0) x = I16(0);
  if (N > 1) y = I16(1);
  int px, py;
  lc_p8_camera(x, y, &px, &py);
  B[0] = LV_INT(px);
  B[1] = LV_INT(py);
  return 2;
}

LCB(clip) {
  int prev[4];
  if (N >= 4) lc_p8_clip(INT(0), INT(1), INT(2), INT(3), 0, 0, prev);
  else lc_p8_clip(0, 0, 128, 128, 0, 1, prev);
  for (int i = 0; i < 4; i++) B[i] = LV_INT(prev[i]);
  return 4;
}

LCB(mget) { RET_INT(lc_p8_mget(INT(0), INT(1))); }
LCB(mset) { lc_p8_mset(INT(0), INT(1), (uint8_t)I16(2)); return 0; }

LCB(map) {
  uint8_t mm = lc_p8mem[P8_MAPMAP];
  int big = mm >= 0x80;
  int size = big ? 0x10000 - (mm << 8) : 8192;
  int mw = lc_p8mem[P8_MAPW] == 0 ? 256 : lc_p8mem[P8_MAPW];
  int mh = size / mw;
  int cx = 0, cy = 0, sx = 0, sy = 0, cw = mw, ch = mh, layer = 0;
  if (N > 0) cx = INT(0);
  if (N > 1) cy = INT(1);
  if (N > 2) sx = INT(2);
  if (N > 3) sy = INT(3);
  if (N > 4) cw = INT(4);
  if (N > 5) ch = INT(5);
  if (N > 6) layer = (uint8_t)I16(6);
  lc_p8_map(cx, cy, sx, sy, cw, ch, layer);
  return 0;
}

LCB(pal) {
  if (N == 0) { lc_p8_pal_reset(-1); return 0; }
  if (LV_TAG(B[0]) == LT_TAB) {
    int p = N > 1 ? (uint8_t)I16(1) : 0;
    lc_tab *t = (lc_tab *)LV_PTR(B[0]);
    lv k = LV_NIL, v;
    while (lc_tab_next(t, &k, &v)) {
      int32_t kn, vn;
      if (lc_tonum(k, &kn) && lc_tonum(v, &vn)) lc_p8_pal((int16_t)(kn >> 16), (int16_t)(vn >> 16), p);
    }
    return 0;
  }
  if (N == 1) { lc_p8_pal_reset((uint8_t)I16(0)); return 0; }
  int16_t c0 = I16(0), c1 = c0, p = 0;
  if (N > 1) c1 = I16(1);
  if (N > 2) p = I16(2);
  RET_INT(lc_p8_pal((uint8_t)c0, (uint8_t)c1, (uint8_t)p));
}

LCB(palt) {
  int16_t prev = 0;
  if (N < 2) {
    int16_t c = (int16_t)0x8000;
    if (N == 1) c = I16(0);
    for (int i = 0; i < 16; i++) {
      int bit = (c >> (15 - i)) & 1;
      if (lc_p8_palt(i, bit)) prev = (int16_t)(prev | (1 << (15 - i)));
    }
  } else prev = (int16_t)lc_p8_palt(I16(0), BOOL(1));
  RET_INT(prev);
}

LCB(fillp) { RET_NUM(lc_p8_fillp(N > 0 ? NUM(0) : 0)); }

/* ---- input ---------------------------------------------------------------------------- */

LCB(btn) {
  if (N == 0) RET_INT(lc_p8_btn(-1, 0));
  int i = INT(0), p = N > 1 ? INT(1) : 0;
  B[0] = LV_BOOL(lc_p8_btn(i, p));
  return 1;
}

LCB(btnp) {
  if (N == 0) RET_INT(lc_p8_btnp(-1, 0));
  int i = INT(0), p = N > 1 ? INT(1) : 0;
  B[0] = LV_BOOL(lc_p8_btnp(i, p));
  return 1;
}

/* ---- system ----------------------------------------------------------------------------- */

LCB(time) { RET_NUM(lc_fdiv((int32_t)(lc_p8_frames << 16), 60 << 16)); }

LCB(stat) {
  int n = INT(0);
  switch (n) {
    case 0: RET_INT(1);
    case 1: case 2: RET_NUM(0x8000);
    case 4: B[0] = LV_STR(lc_str_new("", 0)); return 1;
    case 5: RET_INT(22);
    case 6: B[0] = LV_STR(lc_str_new("", 0)); return 1;
    case 7: case 8: RET_INT(lc_p8_fps);
    case 16: case 17: case 18: case 19: case 20: case 21: case 22: case 23: case 24: case 25: case 26:
    case 46: case 47: case 48: case 49: case 50: case 51: case 52: case 53: case 54: case 55: case 56: case 57:
      RET_INT(lc_p8_audio_stat(n));
    case 28: B[0] = LV_FALSE; return 1;
    case 29: RET_INT(0);
    case 30: B[0] = LV_FALSE; return 1;
    case 31: B[0] = LV_STR(lc_str_new("", 0)); return 1;
    case 32: case 33: case 34: RET_INT(0);
    case 80: case 81: case 82: case 83: case 84: case 85:
    case 90: case 91: case 92: case 93: case 94: case 95: {
      int dt[6];
      lc_plat_datetime(dt);
      RET_INT(dt[n % 10]);
    }
    case 100: B[0] = LV_STR(lc_str_new("", 0)); return 1;
    case 101: B[0] = LV_NIL; return 1;
    case 102: RET_INT(0);
    case 108: RET_INT(32);
    case 120: case 121: case 122: B[0] = LV_FALSE; return 1;
    default: RET_INT(0);
  }
}

LCB(music) {
  int n = INT(0), fade = N > 1 ? INT(1) : 0, mask = N > 2 ? INT(2) : 0;
  lc_p8_music(n, fade, mask);
  return 0;
}

LCB(sfx) {
  int n = INT(0), ch = N > 1 ? INT(1) : -1, off = N > 2 ? INT(2) : 0, len = N > 3 ? INT(3) : -1;
  lc_p8_sfx(n, ch, off, len);
  RET_INT(0);
}

LCB(cstore) { return 0; }

LCB(memcpy) { lc_p8_memcpy(U16(0), U16(1), U16(2)); return 0; }
LCB(memset) { lc_p8_memset(U16(0), (uint8_t)U16(1), U16(2)); return 0; }

LCB(peek) {
  uint16_t a = U16(0);
  int n = 1;
  if (N > 1) { int t = INT(1); if (t > 0) n = t > 8192 ? 8192 : t; }
  if (B + n > lc_stack_hi) lc_error("stack overflow");
  for (int i = 0; i < n; i++) B[i] = LV_INT(p8_rd8((uint32_t)a + (uint32_t)i));
  return n;
}

LCB(poke) {
  uint16_t a = U16(0);
  p8_wr8(a, N > 1 ? (uint8_t)(NUM(1) >> 16) : 0);
  if (N > 2) {
    /* no 8192 cap: that limit is peek's count; zepto8 writes every value and
     * carts poke whole 9 KB tables (FAKE-08 stops at 8192) */
    for (int i = 1; i <= N - 2; i++) p8_wr8((uint32_t)a + (uint32_t)i, (uint8_t)(NUM(1 + i) >> 16));
  }
  return 0;
}

LCB(peek2) { RET_INT(lc_p8_peek2(U16(0))); }
LCB(poke2) {
  uint16_t a = U16(0);
  lc_p8_poke2(a, (int16_t)(N > 1 ? INT(1) : 0));
  /* consecutive values at consecutive addresses, as PICO-8 and zepto8 do
   * (FAKE-08 steps the address by 1 and overlaps them) */
  for (int i = 1; i <= N - 2; i++) lc_p8_poke2((uint32_t)a + 2u * (uint32_t)i, (int16_t)INT(1 + i));
  return 0;
}
LCB(peek4) { RET_NUM(lc_p8_peek4(U16(0))); }
LCB(poke4) {
  uint16_t a = U16(0);
  lc_p8_poke4(a, N > 1 ? NUM(1) : 0);
  for (int i = 1; i <= N - 2; i++) lc_p8_poke4((uint32_t)a + 4u * (uint32_t)i, NUM(1 + i));
  return 0;
}

/* poke/poke2/poke4(addr, unpack(t [, i [, j]])) fused by the compiler, so a
 * large unpack never has to fit on the value stack. U holds unpack's UN
 * arguments; w is the value width in bytes. */
void lc_p8_poke_unpack(int w, int32_t addr, lv *U, int UN) {
  lv tv = lc_arg(U, UN, 0);
  if (LV_TAG(tv) != LT_TAB) lc_error("bad argument #1 to 'unpack' (table expected)");
  lc_tab *t = (lc_tab *)LV_PTR(tv);
  int32_t i = (UN >= 2 && U[1] != LV_NIL) ? (int16_t)(lc_argnum(U, UN, 1) >> 16) : 1;
  int32_t e = (UN >= 3 && U[2] != LV_NIL) ? (int16_t)(lc_argnum(U, UN, 2) >> 16) : LV_N(lc_len(tv)) >> 16;
  int32_t n = i > e ? 0 : e - i + 1;
  uint32_t a = (uint16_t)(int16_t)(addr >> 16);
  if (n == 0) {                              /* poke(a) writes a zero */
    if (w == 1) p8_wr8(a, 0); else if (w == 2) lc_p8_poke2(a, 0); else lc_p8_poke4(a, 0);
    return;
  }
  for (int32_t k = 0; k < n; k++) {
    lv v = lc_tab_getint(t, i + k);
    int32_t x = lc_argnum(&v, 1, 0);
    if (w == 1) p8_wr8(a + (uint32_t)k, (uint8_t)(x >> 16));
    else if (w == 2) lc_p8_poke2(a + 2u * (uint32_t)k, (int16_t)(x >> 16));
    else lc_p8_poke4(a + 4u * (uint32_t)k, x);
  }
}

LCB(reload) {
  int dest = 0, src = 0, len = 0x4300;
  if (N > 0) dest = U16(0);
  if (N > 1) src = U16(1);
  if (N > 2) len = U16(2);
  lc_p8_reload(dest, src, len);      /* reloading from another cart file is not supported */
  return 0;
}

LCB(reset) {
  for (int i = 0; i < 0x7f; i++) lc_p8mem[0x5f00 + i] = 0;
  lc_p8mem[P8_COLORMASK] = 0xff;
  lc_p8mem[P8_SPRMAP] = 0x00;
  lc_p8mem[P8_SCRMAP] = 0x60;
  lc_p8mem[P8_MAPMAP] = 0x20;
  lc_p8mem[P8_MAPW] = 128;
  lc_p8_color(6);
  int prev[4];
  lc_p8_clip(0, 0, 128, 128, 0, 1, prev);
  lc_p8_pal_reset(-1);
  return 0;
}

LCB(cartdata) {
  int r = 0;
  if (N > 0 && (LV_TAG(B[0]) == LT_STR || LV_TAG(B[0]) == LT_NUM)) {
    char nb[16];
    const char *s;
    uint32_t len;
    if (LV_TAG(B[0]) == LT_STR) { s = ((lc_str *)LV_PTR(B[0]))->data; len = ((lc_str *)LV_PTR(B[0]))->len; }
    else { len = (uint32_t)lc_fmtnum(nb, LV_N(B[0])); s = nb; }
    r = lc_p8_cartdata(s, len);
  }
  B[0] = LV_BOOL(r);
  return 1;
}

LCB(dget) {
  int n = INT(0) & 0xff;
  RET_NUM(n < 64 ? lc_p8_peek4(P8_CARTDATA + 4 * (uint32_t)n) : 0);
}
LCB(dset) {
  int n = INT(0) & 0xff;
  if (n < 64) { lc_p8_poke4(P8_CARTDATA + 4 * (uint32_t)n, NUM(1)); lc_p8_cartdata_touch(); }
  return 0;
}

LCB(printh) {
  if (N > 0 && LV_TAG(B[0]) == LT_STR) lc_plat_log(((lc_str *)LV_PTR(B[0]))->data, ((lc_str *)LV_PTR(B[0]))->len);
  else if (N > 0 && LV_TAG(B[0]) == LT_NUM) { char nb[16]; int n = lc_fmtnum(nb, LV_N(B[0])); lc_plat_log(nb, (size_t)n); }
  return 0;
}

LCB(rnd) {
  if (N == 0) RET_NUM(lc_p8_rnd(0x10000));
  if (LV_TAG(B[0]) == LT_TAB) {
    lc_tab *t = (lc_tab *)LV_PTR(B[0]);
    int32_t len = lc_tab_len(t);
    int idx = (lc_p8_rnd(len << 16) >> 16) + 1;
    B[0] = lc_tab_getint(t, idx);
    return 1;
  }
  RET_NUM(lc_p8_rnd(NUM(0)));
}

LCB(srand) { lc_p8_srand(NUM(0)); return 0; }
LCB(flip) { lc_p8_flip(); return 0; }
LCB(run) { lc_p8_restart(); }
LCB(extcmd) {
  if (N > 0 && LV_TAG(B[0]) == LT_STR) {
    lc_str *s = (lc_str *)LV_PTR(B[0]);
    if (s->len == 5 && lc_memcmp(s->data, "reset", 5) == 0) lc_p8_restart();
  }
  return 0;
}
LCB(stop) { for (;;) lc_p8_flip(); }
LCB(menuitem) { return 0; }
LCB(noop) { return 0; }
LCB(serial) { return 0; }

const lc_builtin lc_p8_builtins[] = {
  { "cls", &lcb_cls_clo }, { "pset", &lcb_pset_clo }, { "pget", &lcb_pget_clo },
  { "color", &lcb_color_clo }, { "line", &lcb_line_clo }, { "tline", &lcb_tline_clo },
  { "circ", &lcb_circ_clo }, { "circfill", &lcb_circfill_clo }, { "oval", &lcb_oval_clo },
  { "ovalfill", &lcb_ovalfill_clo }, { "rect", &lcb_rect_clo }, { "rectfill", &lcb_rectfill_clo },
  { "rrect", &lcb_rrect_clo }, { "rrectfill", &lcb_rrectfill_clo }, { "print", &lcb_print_clo },
  { "cursor", &lcb_cursor_clo }, { "spr", &lcb_spr_clo }, { "sspr", &lcb_sspr_clo },
  { "fget", &lcb_fget_clo }, { "fset", &lcb_fset_clo }, { "sget", &lcb_sget_clo },
  { "sset", &lcb_sset_clo }, { "camera", &lcb_camera_clo }, { "clip", &lcb_clip_clo },
  { "mget", &lcb_mget_clo }, { "mset", &lcb_mset_clo }, { "map", &lcb_map_clo },
  { "mapdraw", &lcb_map_clo }, { "pal", &lcb_pal_clo }, { "palt", &lcb_palt_clo },
  { "fillp", &lcb_fillp_clo }, { "btn", &lcb_btn_clo }, { "btnp", &lcb_btnp_clo },
  { "time", &lcb_time_clo }, { "t", &lcb_time_clo }, { "stat", &lcb_stat_clo },
  { "music", &lcb_music_clo }, { "sfx", &lcb_sfx_clo }, { "cstore", &lcb_cstore_clo },
  { "memcpy", &lcb_memcpy_clo }, { "memset", &lcb_memset_clo }, { "peek", &lcb_peek_clo },
  { "poke", &lcb_poke_clo }, { "peek2", &lcb_peek2_clo }, { "poke2", &lcb_poke2_clo },
  { "peek4", &lcb_peek4_clo }, { "poke4", &lcb_poke4_clo }, { "reload", &lcb_reload_clo },
  { "reset", &lcb_reset_clo }, { "cartdata", &lcb_cartdata_clo }, { "dget", &lcb_dget_clo },
  { "dset", &lcb_dset_clo }, { "printh", &lcb_printh_clo }, { "rnd", &lcb_rnd_clo },
  { "srand", &lcb_srand_clo }, { "flip", &lcb_flip_clo }, { "run", &lcb_run_clo },
  { "extcmd", &lcb_extcmd_clo }, { "stop", &lcb_stop_clo }, { "menuitem", &lcb_menuitem_clo },
  { "serial", &lcb_serial_clo }, { "_set_fps", &lcb_noop_clo }, { "trace", &lcb_noop_clo },
  { "save", &lcb_noop_clo }, { "info", &lcb_noop_clo }, { "abort", &lcb_noop_clo },
  { "folder", &lcb_noop_clo }, { "resume", &lcb_noop_clo }, { "reboot", &lcb_noop_clo },
  { "dir", &lcb_noop_clo }, { "ls", &lcb_noop_clo }, { "load", &lcb_noop_clo },
  { 0, 0 },
};
