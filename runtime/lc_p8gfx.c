/* luacretro runtime: PICO-8 graphics.
 *
 * A C port of FAKE-08's graphics.cpp (MIT, jtothebell; its line/tline code
 * comes from tac08 and zepto8). Rasterisation rules are kept exactly - the
 * same circle, oval, rounded-rect and line algorithms, the same clipping
 * and colour-mask order - because the conformance tests diff screen RAM
 * against FAKE-08 byte for byte. The one floating-point expression in the
 * original (tline's minor-axis interpolation) is emulated in integers with
 * float32 rounding so the result is identical without an FPU.
 */
#include "lc_p8.h"

#define DS(a) lc_p8mem[a]

/* ---- buffers ------------------------------------------------------------------------ */

static uint8_t *mapped_buf(uint8_t mapping, uint8_t *fallback) {
  if (mapping == 0) return lc_p8mem + P8_GFX;
  if (mapping == 0x60) return lc_p8mem + P8_SCREEN;
  if (mapping >= 0x80 && LC_P8_MEMSIZE > 0x8000u) return lc_p8mem + ((uint32_t)mapping << 8);
  return fallback;
}
uint8_t *lc_p8_screenbuf(void) { return mapped_buf(DS(P8_SCRMAP), lc_p8mem + P8_SCREEN); }
uint8_t *lc_p8_spritebuf(void) { return mapped_buf(DS(P8_SPRMAP), lc_p8mem + P8_GFX); }

#define CIDX(x, y) (((y) << 6) | ((x) >> 1))

static inline void set_nib(int x, int y, uint8_t v, uint8_t *buf) {
  uint8_t *p = &buf[CIDX(x, y)];
  *p = (x & 1) ? (uint8_t)((*p & 0x0f) | (v << 4)) : (uint8_t)((*p & 0xf0) | (v & 0x0f));
}
static inline uint8_t get_nib(int x, int y, const uint8_t *buf) {
  uint8_t b = buf[CIDX(x, y)];
  return (x & 1) ? (uint8_t)(b >> 4) : (uint8_t)(b & 0x0f);
}

static inline int16_t cam_x(void) { return p8_rd16(P8_CAMERA_X); }
static inline int16_t cam_y(void) { return p8_rd16(P8_CAMERA_Y); }
static inline int clamp_(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static inline int in_clip(int x, int y) {
  return x >= DS(P8_CLIP_XB) && x < DS(P8_CLIP_XE) && y >= DS(P8_CLIP_YB) && y < DS(P8_CLIP_YE);
}

/* ---- pixel writers ------------------------------------------------------------------- */

void lc_p8_plot_sprite(int x, int y, int col) {
  x &= 127;
  y &= 127;
  uint8_t *sb = lc_p8_screenbuf();
  uint8_t c = DS(P8_DRAWPAL + (col & 0x0f)) & 0x0f;
  uint8_t mask = DS(P8_COLORMASK);
  if (mask != 0xff) {
    uint8_t wm = mask & 15, rm = mask >> 4;
    uint8_t src = get_nib(x, y, sb);
    c = (uint8_t)((src & ~wm) | (c & wm & rm));
  }
  set_nib(x, y, c, sb);
}

void lc_p8_plot_pen(int x, int y) {
  x &= 127;
  y &= 127;
  uint8_t *sb = lc_p8_screenbuf();
  uint8_t col = DS(P8_COLOR);
  uint8_t c = col & 0x0f;
  int bit = 15 - ((x & 3) + 4 * (y & 3));
  uint16_t fp = (uint16_t)(DS(P8_FILLP) | (DS(P8_FILLP + 1) << 8));
  if ((fp >> bit) & 1) {
    if (DS(P8_FILLP_T) & 1) return;
    c = (col & 0xf0) >> 4;
  }
  c = DS(P8_DRAWPAL + (c & 0x0f)) & 0x0f;
  uint8_t mask = DS(P8_COLORMASK);
  if (mask != 0xff) {
    uint8_t wm = mask & 15, rm = mask >> 4;
    uint8_t src = get_nib(x, y, sb);
    c = (uint8_t)((src & ~wm) | (c & wm & rm));
  }
  set_nib(x, y, c, sb);
}

static inline void safe_pen(int x, int y) { if (in_clip(x, y)) lc_p8_plot_pen(x, y); }

void lc_p8_set_pen(int col) { DS(P8_COLOR) = (uint8_t)col; }

/* ---- horizontal / vertical spans ------------------------------------------------------ */

static void hline(int x1, int x2, int y) {
  if (!(y >= DS(P8_CLIP_YB) && y < DS(P8_CLIP_YE))) return;
  int xb = DS(P8_CLIP_XB), xe = DS(P8_CLIP_XE);
  if ((x1 < xb && x2 < xb) || (x1 > xe && x2 > xe)) return;
  int maxx = clamp_(x1 > x2 ? x1 : x2, xb, xe - 1);
  int minx = clamp_(x1 < x2 ? x1 : x2, xb, xe - 1);
  int canmemset = DS(P8_COLORMASK) == 0xff && DS(P8_FILLP) == 0 && DS(P8_FILLP + 1) == 0 && maxx - minx > 1;
  if (canmemset) {
    uint8_t *p = lc_p8_screenbuf() + (y * 64);
    uint8_t color = DS(P8_DRAWPAL + (DS(P8_COLOR) & 0x0f)) & 0x0f;
    if (minx & 1) { p[minx / 2] = (uint8_t)((p[minx / 2] & 0x0f) | (color << 4)); ++minx; }
    if ((maxx & 1) == 0) { p[maxx / 2] = (uint8_t)((p[maxx / 2] & 0xf0) | color); --maxx; }
    int n = (maxx - minx + 1) / 2;
    uint8_t v = (uint8_t)(color * 0x11);
    for (int i = 0; i < n; i++) p[minx / 2 + i] = v;
  } else {
    for (int x = minx; x <= maxx; x++) lc_p8_plot_pen(x, y);
  }
}

static void vline(int y1, int y2, int x) {
  if (!(x >= DS(P8_CLIP_XB) && x < DS(P8_CLIP_XE))) return;
  int yb = DS(P8_CLIP_YB), ye = DS(P8_CLIP_YE);
  if ((y1 < yb && y2 < yb) || (y1 > ye && y2 > ye)) return;
  int maxy = clamp_(y1 > y2 ? y1 : y2, yb, ye - 1);
  int miny = clamp_(y1 < y2 ? y1 : y2, yb, ye - 1);
  int skip = DS(P8_COLORMASK) == 0xff && DS(P8_FILLP) == 0 && DS(P8_FILLP + 1) == 0;
  if (skip) {
    uint8_t *sb = lc_p8_screenbuf();
    uint8_t color = DS(P8_DRAWPAL + (DS(P8_COLOR) & 0x0f)) & 0x0f;
    uint8_t mask = (x & 1) ? 0x0f : 0xf0;
    uint8_t nib = (x & 1) ? (uint8_t)(color << 4) : color;
    for (int y = miny; y <= maxy; ++y) {
      uint8_t *d = &sb[CIDX(x, (int16_t)y)];
      *d = (uint8_t)((*d & mask) | nib);
    }
  } else {
    for (int y = miny; y <= maxy; y++) lc_p8_plot_pen(x, y);
  }
}

/* ---- state ------------------------------------------------------------------------------ */

int lc_p8_color(int c) {
  int prev = DS(P8_COLOR);
  DS(P8_COLOR) = (uint8_t)c;
  return prev;
}

void lc_p8_cls(int color) {
  color &= 15;
  uint8_t v = (uint8_t)(color << 4 | color);
  uint8_t *sb = lc_p8_screenbuf();
  for (int i = 0; i < 0x2000; i++) sb[i] = v;
  DS(P8_TEXT_X) = 0;
  DS(P8_TEXT_Y) = 0;
  DS(P8_CLIP_XB) = 0;
  DS(P8_CLIP_YB) = 0;
  DS(P8_CLIP_XE) = 128;
  DS(P8_CLIP_YE) = 128;
}

void lc_p8_pset(int x, int y, int col) {
  if (col >= 0) lc_p8_color(col & 0xff);
  x -= cam_x();
  y -= cam_y();
  if (in_clip(x, y)) lc_p8_plot_pen(x, y);
}

int lc_p8_pget(int x, int y) {
  x -= cam_x();
  y -= cam_y();
  if (x >= 0 && x <= 127 && y >= 0 && y <= 127) return get_nib(x, y, lc_p8_screenbuf());
  return 0;
}

void lc_p8_line_invalidate(void) {
  p8_wr16(P8_LINE_X, 0);
  p8_wr16(P8_LINE_Y, 0);
  DS(P8_LINEINV) = 1;
}

static int iabs(int v) { return v < 0 ? -v : v; }

void lc_p8_line(int x0, int y0, int x1, int y1, int col) {
  p8_wr16(P8_LINE_X, (int16_t)x1);
  p8_wr16(P8_LINE_Y, (int16_t)y1);
  DS(P8_LINEINV) = 0;
  x0 -= cam_x(); y0 -= cam_y();
  x1 -= cam_x(); y1 -= cam_y();
  lc_p8_color(col & 0xff);
  if (x0 == x1) vline(y0, y1, x0);
  else if (y0 == y1) hline(x0, x1, y0);
  else {
    int dx = iabs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -iabs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;
    for (;;) {
      safe_pen(x0, y0);
      if (x0 == x1 && y0 == y1) break;
      e2 = 2 * err;
      if (e2 >= dy) { err += dy; x0 += sx; }
      if (e2 <= dx) { err += dx; y0 += sy; }
    }
  }
}

void lc_p8_line_to(int x1, int y1, int col) {
  if (DS(P8_LINEINV) == 0) lc_p8_line(p8_rd16(P8_LINE_X), p8_rd16(P8_LINE_Y), x1, y1, col);
}

/* ---- tline --------------------------------------------------------------------------------- */

/* base + ((float)num / den) * span, assigned to int: every operation
 * rounded to float32 (round half to even) and the sum truncated toward
 * zero - bit-identical to the C++ expression without an FPU. Values are
 * carried as signed Q48 integers; anything below 2^-48 cannot change the
 * truncated result for the coordinate ranges involved. */
static uint64_t f32_round_mag(uint64_t a) {
  /* round a magnitude (Q48) to a 24-bit mantissa */
  if (a < (1ull << 24)) return a;
  int hb = 63;
  while (!((a >> hb) & 1)) hb--;
  int shift = hb - 23;
  uint64_t q = a >> shift, rem = a & ((1ull << shift) - 1), half = 1ull << (shift - 1);
  if (rem > half || (rem == half && (q & 1))) q++;
  return q << shift;
}

static int interp_f32(int base, int num, int den, int span) {
  if (den == 0) return base;                       /* not reachable from tline */
  int neg = 0;
  uint64_t n = (uint64_t)(num < 0 ? -num : num), d = (uint64_t)(den < 0 ? -den : den);
  if ((num < 0) != (den < 0)) neg ^= 1;
  /* q = n/d correctly rounded to float32, as Q48 */
  uint64_t q = 0;
  if (n) {
    /* scale so the quotient has at least 25 significant bits, then round */
    int sh = 48;
    uint64_t hi = (n << 16) / d;                    /* Q16 quotient, n < 2^16 */
    uint64_t rem = (n << 16) % d;
    /* extend to Q48 with 32 more bits of long division */
    for (int i = 0; i < 32; i++) { rem <<= 1; hi <<= 1; if (rem >= d) { rem -= d; hi |= 1; } }
    (void)sh;
    /* hi is floor(n/d * 2^48); sticky bit from rem */
    q = f32_round_mag((hi << 1) | (rem != 0)) ;
    q >>= 1;
    /* the sticky trick: rounding on (2*hi + sticky) then halving keeps ties exact */
  }
  uint64_t sp = (uint64_t)(span < 0 ? -span : span);
  if (span < 0) neg ^= 1;
  /* p = q * span rounded (q has 24 significant bits, span < 2^16: exact product fits) */
  uint64_t p = f32_round_mag(q * sp);
  int64_t v = neg ? -(int64_t)p : (int64_t)p;
  int64_t s = ((int64_t)base << 48) + v;
  uint64_t mag = (uint64_t)(s < 0 ? -s : s);
  mag = f32_round_mag(mag);
  int64_t t = (int64_t)(mag >> 48);
  return (int)(s < 0 ? -t : t);
}

static int32_t tline_mask(uint8_t dim) {
  if (dim == 0) return 0xffffff;
  return (int32_t)((uint32_t)dim << 16) - 1;
}

static int clampx(int x) { return clamp_(x, DS(P8_CLIP_XB), DS(P8_CLIP_XE) - 1); }
static int clampy(int y) { return clamp_(y, DS(P8_CLIP_YB), DS(P8_CLIP_YE) - 1); }

void lc_p8_tline(int x0, int y0, int x1, int y1, int32_t mx, int32_t my, int32_t mdx, int32_t mdy, int layers) {
  (void)layers;
  x0 -= cam_x(); y0 -= cam_y();
  x1 -= cam_x(); y1 -= cam_y();
  int xgreater = iabs(x1 - x0) >= iabs(y1 - y0);
  int dx = xgreater ? (x0 <= x1 ? 1 : -1) : 0;
  int dy = xgreater ? 0 : (y0 <= y1 ? 1 : -1);
  int vertical = x0 == x1;
  int x = clampx(x0), xend = clampx(x1);
  int y = clampy(y0), yend = clampy(y1);
  int32_t xmask = tline_mask(DS(P8_TLINE_W));
  int32_t ymask = tline_mask(DS(P8_TLINE_H));
  int delta = iabs(xgreater ? x - x0 : y - y0);
  while (delta) {
    int step = delta < 8192 ? delta : 8192;
    int32_t sb = step << 16;
    mx = (mx & ~xmask) | ((int32_t)((uint32_t)mx + (uint32_t)(int32_t)(((int64_t)mdx * sb) >> 16)) & xmask);
    my = (my & ~ymask) | ((int32_t)((uint32_t)my + (uint32_t)(int32_t)(((int64_t)mdy * sb) >> 16)) & ymask);
    delta -= step;
  }
  const uint8_t *sheet = lc_p8mem + P8_GFX;
  for (;;) {
    int sx = DS(P8_TLINE_X) + ((mx & xmask) >> 16);
    int sy = DS(P8_TLINE_Y) + ((my & ymask) >> 16);
    int sprite = lc_p8_mget(sx, sy);
    if (sprite) {
      int spr_x = (sprite % 16) * 8, spr_y = (sprite / 16) * 8;
      uint8_t col = get_nib(spr_x + (((int32_t)((uint32_t)mx << 3) >> 16) & 7),
                            spr_y + (((int32_t)((uint32_t)my << 3) >> 16) & 7), sheet);
      if (!(DS(P8_DRAWPAL + (col & 15)) >> 4) && in_clip(x, y)) lc_p8_plot_sprite(x, y, col);
    }
    mx = (mx & ~xmask) | ((int32_t)((uint32_t)mx + (uint32_t)mdx) & xmask);
    my = (my & ~ymask) | ((int32_t)((uint32_t)my + (uint32_t)mdy) & ymask);
    if (xgreater) {
      if (x == xend) break;
      x += dx;
      y = interp_f32(y0, x - x0, x1 - x0, y1 - y0);
    } else {
      if (y == yend) break;
      y += dy;
      if (!vertical) x = interp_f32(x0, y - y0, y1 - y0, x1 - x0);
    }
  }
}

/* ---- circles / ovals ------------------------------------------------------------------------ */

void lc_p8_circ(int ox, int oy, int r, int col) {
  lc_p8_color(col & 0xff);
  ox -= cam_x(); oy -= cam_y();
  int x = r, y = 0, d = 1 - x;
  while (y <= x) {
    safe_pen(ox + x, oy + y); safe_pen(ox + y, oy + x);
    safe_pen(ox - x, oy + y); safe_pen(ox - y, oy + x);
    safe_pen(ox - x, oy - y); safe_pen(ox - y, oy - x);
    safe_pen(ox + x, oy - y); safe_pen(ox + y, oy - x);
    y += 1;
    if (d < 0) d = d + 2 * y + 1;
    else { x = x - 1; d = d + 2 * (y - x) + 1; }
  }
}

static void inverted_circfill(int ox, int oy, int r, int col) {
  lc_p8_color(col & 0xff);
  ox -= cam_x(); oy -= cam_y();
  int cl = DS(P8_CLIP_XB), cr = DS(P8_CLIP_XE), ct = DS(P8_CLIP_YB), cb = DS(P8_CLIP_YE);
  int top = oy - r, bottom = oy + r;
  for (int row = ct; row < top && row < cb; row++) hline(cl, cr - 1, row);
  if (r >= 0) {
    int x = -r, y = 0, err = 2 - 2 * r, lastY = -1;
    do {
      if (oy + y != lastY) {
        if (ox + x + 1 < cr) hline(ox - x + 1, cr - 1, oy + y);
        if (ox + x - 1 >= cl) hline(cl, ox + x - 1, oy + y);
      }
      if (y > 0 && oy - y != lastY) {
        if (ox + x + 1 < cr) hline(ox - x + 1, cr - 1, oy - y);
        if (ox + x - 1 >= cl) hline(cl, ox + x - 1, oy - y);
      }
      lastY = oy + y;
      int saved = r;
      r = err;
      if (r > x) err += ++x * 2 + 1;
      if (r <= y) err += ++y * 2 + 1;
      r = saved;
    } while (x < 0);
  }
  for (int row = bottom + 1; row < cb; row++) hline(cl, cr - 1, row);
}

void lc_p8_circfill(int ox, int oy, int r, int col) {
  if (DS(P8_COLORFLAG) & 0x02) { inverted_circfill(ox, oy, r, col); return; }
  lc_p8_color(col & 0xff);
  ox -= cam_x(); oy -= cam_y();
  if (r == 0) safe_pen(ox, oy);
  else if (r == 1) {
    safe_pen(ox, oy - 1);
    hline(ox - 1, ox + 1, oy);
    safe_pen(ox, oy + 1);
  } else if (r > 0) {
    int x = -r, y = 0, err = 2 - 2 * r;
    do {
      hline(ox - x, ox + x, oy + y);
      hline(ox - x, ox + x, oy - y);
      r = err;
      if (r > x) err += ++x * 2 + 1;
      if (r <= y) err += ++y * 2 + 1;
    } while (x < 0);
  }
}

static void sort_rect(int *x0, int *y0, int *x1, int *y1) {
  if (*x0 > *x1) { int t = *x0; *x0 = *x1; *x1 = t; }
  if (*y0 > *y1) { int t = *y0; *y0 = *y1; *y1 = t; }
}

static void oval_impl(int x0, int y0, int x1, int y1, int col, int fill) {
  lc_p8_color(col & 0xff);
  x0 -= cam_x(); y0 -= cam_y();
  x1 -= cam_x(); y1 -= cam_y();
  sort_rect(&x0, &y0, &x1, &y1);
  int xr = (x1 - x0) / 2, yr = (y1 - y0) / 2;
  int xc = x0 + xr, yc = y0 + yr;
  int wx, wy, thresh;
  int asq = xr * xr, bsq = yr * yr;
  int xa, ya;
  if (fill) vline(yc + yr, yc - yr, xc);
  else { safe_pen(xc, yc + yr); safe_pen(xc, yc - yr); }
  wx = 0; wy = yr; xa = 0; ya = asq * 2 * yr;
  thresh = asq / 4 - asq * yr;
  for (;;) {
    thresh += xa + bsq;
    if (thresh >= 0) { ya -= asq * 2; thresh -= ya; wy--; }
    xa += bsq * 2;
    wx++;
    if (xa >= ya) break;
    if (fill) { hline(xc + wx, xc - wx, yc - wy); hline(xc + wx, xc - wx, yc + wy); }
    else {
      safe_pen(xc + wx, yc - wy); safe_pen(xc - wx, yc - wy);
      safe_pen(xc + wx, yc + wy); safe_pen(xc - wx, yc + wy);
    }
  }
  if (fill) hline(xc + xr, xc - xr, yc);
  else { safe_pen(xc + xr, yc); safe_pen(xc - xr, yc); }
  wx = xr; wy = 0; xa = bsq * 2 * xr; ya = 0;
  thresh = bsq / 4 - bsq * xr;
  for (;;) {
    thresh += ya + asq;
    if (thresh >= 0) { xa -= bsq * 2; thresh = thresh - xa; wx--; }
    ya += asq * 2;
    wy++;
    if (ya > xa) break;
    if (ya == 0 && xa == 0) break;
    if (fill) { hline(xc + wx, xc - wx, yc - wy); hline(xc + wx, xc - wx, yc + wy); }
    else {
      safe_pen(xc + wx, yc - wy); safe_pen(xc - wx, yc - wy);
      safe_pen(xc + wx, yc + wy); safe_pen(xc - wx, yc + wy);
    }
  }
}
void lc_p8_oval(int x0, int y0, int x1, int y1, int col) { oval_impl(x0, y0, x1, y1, col, 0); }
void lc_p8_ovalfill(int x0, int y0, int x1, int y1, int col) { oval_impl(x0, y0, x1, y1, col, 1); }

/* ---- rects ------------------------------------------------------------------------------------ */

void lc_p8_rect(int x1, int y1, int x2, int y2, int col) {
  DS(P8_COLOR) = (uint8_t)col;
  x1 -= cam_x(); y1 -= cam_y();
  x2 -= cam_x(); y2 -= cam_y();
  sort_rect(&x1, &y1, &x2, &y2);
  hline(x1, x2, y1);
  hline(x1, x2, y2);
  vline(y1, y2, x1);
  vline(y1, y2, x2);
}

void lc_p8_rectfill(int x1, int y1, int x2, int y2, int col) {
  DS(P8_COLOR) = (uint8_t)col;
  x1 -= cam_x(); y1 -= cam_y();
  x2 -= cam_x(); y2 -= cam_y();
  sort_rect(&x1, &y1, &x2, &y2);
  for (int y = y1; y <= y2; y++) hline(x1, x2, y);
}

static int rrect_cut(int radius, int row) {
  static const int8_t c6[] = { 5, 3, 2, 1, 1, 0 };
  static const int8_t c7[] = { 6, 4, 3, 2, 1, 1, 0 };
  static const int8_t c8[] = { 7, 5, 3, 2, 2, 1, 1, 0 };
  static const int8_t c9[] = { 7, 5, 4, 3, 2, 1, 1, 0 };
  static const int8_t c11[] = { 9, 7, 5, 4, 3, 2, 2, 1, 1, 0, 0 };
  static const int8_t c13[] = { 11, 8, 7, 5, 4, 3, 3, 2, 1, 1, 1, 0, 0 };
  static const int8_t c15[] = { 13, 10, 8, 7, 5, 4, 4, 3, 2, 2, 1, 1, 1, 0, 0 };
  if (radius == 6) return row < 6 ? c6[row] : 0;
  if (radius == 7) return row < 7 ? c7[row] : 0;
  if (radius == 8) return row < 8 ? c8[row] : 0;
  if (radius == 9 || radius == 10) return row < 8 ? c9[row] : 0;
  if (radius == 11 || radius == 12) return row < 11 ? c11[row] : 0;
  if (radius == 13 || radius == 14) return row < 13 ? c13[row] : 0;
  if (radius == 15 || radius == 16) return row < 15 ? c15[row] : 0;
  if (row >= radius) return 0;
  /* x = r - sqrt(r^2 - y^2); cut = (int)(x + 0.5) - 1, done exactly:
   * floor(r + 0.5 - s) = largest k with 4D <= (2r + 1 - 2k)^2, 2r+1-2k >= 0 */
  int64_t yv = radius - row - 1;
  int64_t D = (int64_t)radius * radius - yv * yv;
  if (D <= 0) return 0;
  int k = 0;
  for (int t = radius; t >= 0; t--) {
    int64_t w = 2 * (int64_t)radius + 1 - 2 * (int64_t)t;
    if (w >= 0 && 4 * D <= w * w) { k = t; break; }
  }
  int cut = k - 1;
  if (cut == 0 && row < radius - 1) {
    int tail = radius - row;        /* tail_y <= 2.5 */
    if (tail <= 2) cut = 1;
  }
  return cut < 0 ? 0 : cut;
}

void lc_p8_rrect(int x, int y, int w, int h, int r, int col) {
  lc_p8_color(col & 0xff);
  int maxr = (w < h ? w : h) / 2;
  if (r > maxr) r = maxr;
  if (r <= 0) { lc_p8_rect(x, y, x + w - 1, y + h - 1, col & 0xff); return; }
  x -= cam_x(); y -= cam_y();
  for (int row = 0; row < h; row++) {
    int cut = 0;
    if (row < r) {
      if (r <= 5) cut = r - 1 - row;
      else {
        int rsq = r * r, maxX = 0;
        for (int tx = 0; tx < r; tx++) if (tx * tx + row * row <= rsq) maxX = tx;
        cut = r - 1 - maxX;
      }
    } else if (row >= h - r) {
      int br = h - 1 - row;
      cut = r <= 5 ? r - 1 - br : rrect_cut(r, br);
    }
    if (cut < 0) cut = 0;
    if (cut * 2 >= w) cut = (w - 1) / 2;
    int ls = x + cut, le = x + w - 1 - cut;
    if (ls <= le) {
      if (row == 0 || row == h - 1) hline(ls, le, y + row);
      else { safe_pen(ls, y + row); if (ls != le) safe_pen(le, y + row); }
    }
  }
}

void lc_p8_rrectfill(int x, int y, int w, int h, int r, int col) {
  lc_p8_color(col & 0xff);
  int maxr = (w < h ? w : h) / 2;
  if (r > maxr) r = maxr;
  if (r <= 0) { lc_p8_rectfill(x, y, x + w - 1, y + h - 1, col & 0xff); return; }
  x -= cam_x(); y -= cam_y();
  for (int row = 0; row < h; row++) {
    int cut = 0;
    if (row < r) cut = r <= 5 ? r - 1 - row : rrect_cut(r, row);
    else if (row >= h - r) {
      int br = h - 1 - row;
      cut = r <= 5 ? r - 1 - br : rrect_cut(r, br);
    }
    if (cut < 0) cut = 0;
    if (cut * 2 >= w) cut = (w - 1) / 2;
    int ls = x + cut, le = x + w - 1 - cut;
    if (ls <= le) hline(ls, le, y + row);
  }
}

/* ---- sprites ------------------------------------------------------------------------------------- */

static void blit(const uint8_t *sprbuf, int scr_x, int scr_y, int spr_x, int spr_y,
                 int spr_w, int spr_h, int flip_x, int flip_y) {
  int scr_w = spr_w, scr_h = spr_h;
  uint8_t *sb = lc_p8_screenbuf();
  uint8_t mask = DS(P8_COLORMASK);
  uint8_t wm = mask & 15, rm = mask >> 4;
  int cxb = DS(P8_CLIP_XB), cxe = DS(P8_CLIP_XE), cyb = DS(P8_CLIP_YB), cye = DS(P8_CLIP_YE);
  scr_x -= cam_x();
  scr_y -= cam_y();
  if (scr_x < cxb) {
    int n = cxb - scr_x;
    scr_x = cxb; scr_w -= n;
    if (!flip_x) spr_x += n; else spr_w -= n;
  }
  if (scr_x + scr_w > cxe) scr_w -= (scr_x + scr_w) - cxe;
  if (scr_y < cyb) {
    int n = cyb - scr_y;
    scr_y = cyb; scr_h -= n;
    if (!flip_y) spr_y += n; else spr_h -= n;
  }
  if (scr_y + scr_h > cye) scr_h -= (scr_y + scr_h) - cye;

  /* FAKE-08 walks the sprite two pixels (one byte) at a time and pairs the
   * low/high nibbles; when spr_x is odd the first byte contributes only its
   * high nibble. The write positions below reproduce that pairing. */
  int start_half = spr_x & 1;
  for (int y = 0; y < scr_h; y++) {
    int x = 0;
    while (x < scr_w) {
      int ax = spr_x + (flip_x ? spr_w - (x + 1) : x);
      int ay = spr_y + (flip_y ? spr_h - (y + 1) : y);
      if (!(ay >= 0 && ay < 128 && ax >= 0 && ax < 128)) { ++x; continue; }
      uint8_t both = sprbuf[CIDX(ax, ay)];
      uint8_t lc = both & 0x0f, rc = (both >> 4) & 0x0f;
      int fy = scr_y + y;
      int fx = scr_x + (flip_x ? x + 1 : x);
      if (x > 0 || !start_half) {
        uint8_t m = DS(P8_DRAWPAL + lc);
        if (!(m >> 4)) {
          uint8_t c = m & 0x0f;
          uint8_t src = get_nib(fx, fy, sb);
          c = (uint8_t)((src & ~wm) | (c & wm & rm));
          set_nib(fx, fy, c, sb);
        }
        ++x;
        if (flip_x) --fx; else ++fx;
      }
      if (x < scr_w) {
        uint8_t m = DS(P8_DRAWPAL + rc);
        if (!(m >> 4)) {
          uint8_t c = m & 0x0f;
          uint8_t src = get_nib(fx, fy, sb);
          c = (uint8_t)((src & ~wm) | (c & wm & rm));
          set_nib(fx, fy, c, sb);
        }
        ++x;
      }
    }
  }
}

static void stretch_blit(const uint8_t *sprbuf, int spr_x, int spr_y, int spr_w, int spr_h,
                         int scr_x, int scr_y, int scr_w, int scr_h, int flip_x, int flip_y) {
  if (scr_w == 0 || scr_h == 0) return;
  if (spr_h == scr_h && spr_w == scr_w && !flip_x) {
    blit(sprbuf, scr_x, scr_y, spr_x, spr_y, scr_w, scr_h, flip_x, flip_y);
    return;
  }
  uint8_t *sb = lc_p8_screenbuf();
  uint8_t mask = DS(P8_COLORMASK);
  uint8_t wm = mask & 15, rm = mask >> 4;
  int cxb = DS(P8_CLIP_XB), cxe = DS(P8_CLIP_XE), cyb = DS(P8_CLIP_YB), cye = DS(P8_CLIP_YE);
  scr_x -= cam_x();
  scr_y -= cam_y();
  if (scr_w < 0) { flip_x = !flip_x; scr_w = -scr_w; scr_x -= scr_w; }
  if (scr_h < 0) { flip_y = !flip_y; scr_h = -scr_h; scr_y -= scr_h; }
  spr_x <<= 16; spr_y <<= 16; spr_w <<= 16; spr_h <<= 16;
  int dx = spr_w / scr_w, dy = spr_h / scr_h;
  if (scr_x < cxb) {
    int n = cxb - scr_x;
    scr_x = cxb; scr_w -= n;
    if (!flip_x) spr_x += n * dx; else spr_w -= n * dx;
  }
  if (scr_x + scr_w > cxe) scr_w -= (scr_x + scr_w) - cxe;
  if (scr_y < cyb) {
    int n = cyb - scr_y;
    scr_y = cyb; scr_h -= n;
    if (!flip_y) spr_y += n * dy; else spr_h -= n * dy;
  }
  if (scr_y + scr_h > cye) scr_h -= (scr_y + scr_h) - cye;
  if (flip_y) { spr_y += spr_h - 1 * dy; dy = -dy; }
  int fast = mask == 0xff;
  for (int y = 0; y < scr_h; y++) {
    int sy = (spr_y + y * dy) >> 16;
    if (sy > 127 || sy < 0) continue;              /* FAKE-08 reads out of bounds for sy < 0 */
    const uint8_t *row = sprbuf + sy * 64;
    for (int x = 0; x < scr_w; x++) {
      int pix, sx;
      if (!flip_x) { pix = spr_x + x * dx; sx = pix >> 16; }
      else { pix = spr_x + spr_w - (x + 1) * dx; sx = pix >> 16; }
      if (sx > 127 || sx < 0) continue;
      uint8_t both = row[sx / 2];
      uint8_t c = (sx % 2 == 0) ? (both & 0x0f) : (uint8_t)((both >> 4) & 0x0f);
      uint8_t m = DS(P8_DRAWPAL + c);
      if (m >> 4) continue;
      c = m & 0x0f;
      int fx = scr_x + x, fy = scr_y + y;
      if (!fast) {
        uint8_t src = get_nib(fx, fy, sb);
        c = (uint8_t)((src & ~wm) | (c & wm & rm));
      }
      set_nib(fx, fy, c, sb);
    }
  }
}

void lc_p8_spr(int n, int x, int y, int32_t w, int32_t h, int flipx, int flipy) {
  int spr_x = (n % 16) * 8, spr_y = (n / 16) * 8;
  int16_t spr_w = (int16_t)(((int32_t)(((int64_t)w * (8 << 16)) >> 16)) >> 16);
  int16_t spr_h = (int16_t)(((int32_t)(((int64_t)h * (8 << 16)) >> 16)) >> 16);
  blit(lc_p8_spritebuf(), x, y, spr_x, spr_y, spr_w, spr_h, flipx, flipy);
}

void lc_p8_sspr(int sx, int sy, int sw, int sh, int dx, int dy, int dw, int dh, int flipx, int flipy) {
  stretch_blit(lc_p8_spritebuf(), sx, sy, sw, sh, dx, dy, dw, dh, flipx, flipy);
}

/* ---- sprite sheet / flags / map ---------------------------------------------------------------------- */

int lc_p8_fget(int n) { return DS(P8_GFF + (n & 0xff)); }
void lc_p8_fset(int n, int v) { DS(P8_GFF + (n & 0xff)) = (uint8_t)v; }

int lc_p8_sget(int x, int y) {
  x &= 0xff; y &= 0xff;                 /* (uint8_t) casts in the API */
  if (x < 128 && y < 128) return get_nib(x, y, lc_p8_spritebuf());
  return 0;
}
void lc_p8_sset(int x, int y, int c) {
  x &= 0xff; y &= 0xff;
  if (x >= 0 && x < 128 && y >= 0 && y < 128) set_nib(x, y, (uint8_t)c, lc_p8_spritebuf());
}

static void map_geom(int *big, int *size, int *w, int *h) {
  uint8_t mm = DS(P8_MAPMAP);
  *big = mm >= 0x80;
  *size = *big ? 0x10000 - (mm << 8) : 8192;
  *w = DS(P8_MAPW) == 0 ? 256 : DS(P8_MAPW);
  *h = *size / *w;
}

int lc_p8_mget(int cx, int cy) {
  int big, size, w, h;
  map_geom(&big, &size, &w, &h);
  if (cx < 0 || cx > w - 1 || cy < 0 || cy > h - 1) return 0;
  int idx = cy * w + cx;
  if (big) {
    uint32_t a = 0x8000u + (uint32_t)(0x8000 - size) + (uint32_t)idx;
    return p8_rd8(a);
  }
  if (idx < 4096) return DS(P8_MAP + idx);
  if (idx < 8192) return DS(P8_GFX + idx);
  return 0;
}

void lc_p8_mset(int cx, int cy, int v) {
  int big, size, w, h;
  map_geom(&big, &size, &w, &h);
  if (cx < 0 || cx > w - 1 || cy < 0 || cy > h - 1) return;
  int idx = cy * w + cx;
  if (big) { p8_wr8(0x8000u + (uint32_t)(0x8000 - size) + (uint32_t)idx, (uint8_t)v); return; }
  if (idx < 4096) DS(P8_MAP + idx) = (uint8_t)v;
  else if (idx < 8192) DS(P8_GFX + idx) = (uint8_t)v;
}

void lc_p8_map(int cx, int cy, int sx, int sy, int cw, int ch, int layer) {
  for (int y = 0; y < ch; y++) {
    for (int x = 0; x < cw; x++) {
      int cell = lc_p8_mget(cx + x, cy + y);
      if (cell && (layer == 0 || (lc_p8_fget(cell) & layer)))
        lc_p8_spr(cell, sx + x * 8, sy + y * 8, 0x10000, 0x10000, 0, 0);
    }
  }
}

/* ---- palette / state ------------------------------------------------------------------------------------ */

void lc_p8_palt_reset(void) {
  DS(P8_DRAWPAL) |= 0x10;
  for (int c = 1; c < 16; c++) DS(P8_DRAWPAL + c) &= (uint8_t)~0x10;
}

int lc_p8_palt(int c, int t) {
  c &= 15;
  int prev = (DS(P8_DRAWPAL + c) & 0xf0) != 0;
  if (t) DS(P8_DRAWPAL + c) |= 0x10;
  else DS(P8_DRAWPAL + c) &= (uint8_t)~0x10;
  return prev;
}

void lc_p8_pal_reset(int which) {
  if (which < 0) {
    for (int c = 0; c < 16; c++) { DS(P8_DRAWPAL + c) = (uint8_t)c; DS(P8_SCREENPAL + c) = (uint8_t)c; }
    lc_p8_palt_reset();
    return;
  }
  for (int c = 0; c < 16; c++) {
    if (which == 0) {
      uint8_t tb = DS(P8_DRAWPAL + c) & 0x10;
      if (c == 0 && tb == 0) tb = 0x10;
      DS(P8_DRAWPAL + c) = (uint8_t)(c | tb);
    } else if (which == 1) DS(P8_SCREENPAL + c) = (uint8_t)c;
  }
}

int lc_p8_pal(int c0, int c1, int p) {
  c0 &= 0x0f;
  int prev = 0;
  p &= 0xff;
  if (p == 0) {
    prev = DS(P8_DRAWPAL + c0) & 0xf;
    DS(P8_DRAWPAL + c0) = (uint8_t)((DS(P8_DRAWPAL + c0) & 0x10) | (c1 & 0xf));
  } else if (p == 1) {
    prev = DS(P8_SCREENPAL + c0) & 0xf;
    DS(P8_SCREENPAL + c0) = (uint8_t)(c1 & 0x8f);
  }
  return prev;
}

int32_t lc_p8_fillp(int32_t pat) {
  int32_t prev = (DS(P8_FILLP) << 16) | (int32_t)((uint32_t)DS(P8_FILLP + 1) << 24) | (DS(P8_FILLP_T) << 8);
  DS(P8_FILLP) = (uint8_t)(pat >> 16);
  DS(P8_FILLP + 1) = (uint8_t)(pat >> 24);
  DS(P8_FILLP_T) = (uint8_t)((pat >> 15) & 1);
  return prev;
}

void lc_p8_camera(int x, int y, int *px, int *py) {
  *px = cam_x();
  *py = cam_y();
  p8_wr16(P8_CAMERA_X, (int16_t)x);
  p8_wr16(P8_CAMERA_Y, (int16_t)y);
}

void lc_p8_clip(int x, int y, int w, int h, int clip_prev, int reset, int *prev4) {
  (void)clip_prev;
  prev4[0] = DS(P8_CLIP_XB);
  prev4[1] = DS(P8_CLIP_XE);
  prev4[2] = DS(P8_CLIP_YB);
  prev4[3] = DS(P8_CLIP_YE);
  if (reset) { x = 0; y = 0; w = 128; h = 128; }
  int xe = x + w, ye = y + h;
  DS(P8_CLIP_XB) = (uint8_t)clamp_(x, 0, 128);
  DS(P8_CLIP_YB) = (uint8_t)clamp_(y, 0, 128);
  DS(P8_CLIP_XE) = (uint8_t)clamp_(xe, 0, 128);
  DS(P8_CLIP_YE) = (uint8_t)clamp_(ye, 0, 128);
}

void lc_p8_cursor(int x, int y, int col, int *px, int *py, int *pcol) {
  *px = DS(P8_TEXT_X);
  *py = DS(P8_TEXT_Y);
  *pcol = DS(P8_COLOR);
  if (col >= 0) lc_p8_color(col & 0xff);
  DS(P8_TEXT_X) = (uint8_t)x;
  DS(P8_TEXT_Y) = (uint8_t)y;
}

void lc_p8_reset_drawstate(void) {
  /* clip() camera() pal() color() fillp() */
  int prev[4];
  lc_p8_clip(0, 0, 128, 128, 0, 1, prev);
  int a, b;
  lc_p8_camera(0, 0, &a, &b);
  lc_p8_pal_reset(-1);
  lc_p8_color(6);
  lc_p8_fillp(0);
}
