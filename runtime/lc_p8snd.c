/* luacretro runtime: PICO-8 audio (sfx, music, waveform synthesis).
 *
 * A fixed-point port of FAKE-08's Audio.cpp / synth.cpp / filter.cpp
 * (MIT, jtothebell), which follow zepto8's sfx player (WTFPL, Sam
 * Hocevar). Control flow mirrors Audio::FillMonoAudioBuffer line for line
 * so the two stay diffable; the conformance test renders both and compares.
 *
 * No float anywhere: the targets include a 16.8 MHz ARM7 and a 7.6 MHz
 * 68000 with no FPU. Formats:
 *   dbl        a non-negative IEEE double kept as (53-bit mantissa, exponent).
 *              FAKE-08 advances note offsets by adding a double every sample;
 *              about half the note boundaries land one sample late through
 *              that rounding, and a one-sample slip is a lasting phase offset.
 *              So offsets are those exact doubles: the per-sample add is one
 *              64-bit add plus a rounding check, the full soft add only runs
 *              when the value crosses a power of two.
 *   phi        oscillator phase, u64 Q32.32, rounded to 24 significant bits
 *              after every add because the reference's phase is a float that
 *              grows through a whole held note (its pitch drifts with it).
 *   freq       the reference's float frequency in Hz, bit-exact, through a
 *              small soft-float (mul/add/div): see "soft float32" below.
 *   samples    int32 Q24 through the whole channel pipeline (the final
 *              (int16)(32767.99f * v) is reproduced with the float's rounding,
 *              which hardware distort's /0x1000 quantizer is sensitive to);
 *              volumes Q30 (a volume that is merely tiny in the reference must
 *              stay nonzero: it decides whether the noise generator runs),
 *              music fade Q30, note-change crossfade Q24.
 *   reverb     one int16 Q14 ring of 732 samples per channel serves both the
 *              366- and 732-sample taps (FAKE-08 keeps two float rings).
 *   damping    biquads, Q14 int16 coefficients over Q20 state, each product
 *              split into two 16x16 multiplies (no 64-bit math). FAKE-08 runs
 *              both filters on every channel every sample; here they only run
 *              while their output is mixed in, and are re-primed from the reverb
 *              ring (which holds exactly their input history) when switched on.
 * No 64-bit division anywhere; per sample it is adds, shifts and 32x32->64
 * multiplies. The soft double/float work is cached (per speed, per frequency)
 * or confined to effect notes, and silent channels cost little.
 *
 * Noise uses the runtime's own xorshift where FAKE-08 calls rand(); the
 * conformance harness feeds FAKE-08 the same generator.
 */
#include "lc_p8.h"

/* Per-sample functions carry LC_P8SND_HOT so a platform can place them, e.g.
 * on GBA: -DLC_P8SND_HOT='__attribute__((section(".iwram"), target("arm")))'
 * (ARM code in 32-bit IWRAM: 64-bit adds and 32x32->64 multiplies inline). */
#ifndef LC_P8SND_HOT
#define LC_P8SND_HOT
#endif

/* ---- soft doubles (non-negative) ------------------------------------------------ */
typedef struct { uint64_t m; int32_t e; } dbl;   /* m * 2^e; m == 0 or 2^52 <= m < 2^53 */

#define M52 ((uint64_t)1 << 52)
#define M53 ((uint64_t)1 << 53)

/* Round s * 2^e (plus a sticky bit below it) to 53 bits, ties to even. */
static dbl d_round(uint64_t s, int32_t e, int sticky) {
  dbl r;
  int rb = 0;
  if (!s) { r.m = 0; r.e = 0; return r; }
  while (s < M52) { s <<= 1; e--; }
  while (s >= M53) { sticky |= rb; rb = (int)(s & 1); s >>= 1; e++; }
  if (rb && (sticky || (s & 1))) { s++; if (s == M53) { s >>= 1; e++; } }
  r.m = s; r.e = e;
  return r;
}

static dbl d_int(uint32_t n) { return d_round(n, 0, 0); }

static dbl d_add(dbl a, dbl b) {
  uint64_t x, y, yy;
  int sh, st = 0;
  if (!a.m) return b;
  if (!b.m) return a;
  if (a.e < b.e) { dbl t = a; a = b; b = t; }
  sh = a.e - b.e;
  x = a.m << 10;
  if (sh >= 63) { y = 0; st = 1; }
  else { yy = b.m << 10; y = yy >> sh; st = sh && (yy & (((uint64_t)1 << sh) - 1)) != 0; }
  return d_round(x + y, a.e - 10, st);
}

/* a += d, the per-sample path: stays inside a's binade with one add. */
LC_P8SND_HOT static void d_step(dbl *a, const dbl *d) {
  if (a->m && a->e >= d->e) {
    int sh = a->e - d->e;
    uint64_t dint, rem, half, s;
    if (sh >= 54) return;                 /* d is under half an ulp of a */
    dint = d->m >> sh;
    s = a->m + dint;
    if (sh) {
      rem = d->m & (((uint64_t)1 << sh) - 1);
      half = (uint64_t)1 << (sh - 1);
      if (rem > half || (rem == half && (s & 1))) s++;
    }
    if (s < M53) { a->m = s; return; }
  }
  *a = d_add(*a, *d);
}

static dbl d_mul(dbl a, dbl b) {
  uint64_t a0 = (uint32_t)a.m, a1 = a.m >> 32, b0 = (uint32_t)b.m, b1 = b.m >> 32;
  uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
  uint64_t mid = (p00 >> 32) + (uint32_t)p01 + (uint32_t)p10;
  uint64_t lo = (uint32_t)p00 | (mid << 32);
  uint64_t hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
  if (!a.m || !b.m) return d_int(0);
  return d_round((hi << 14) | (lo >> 50), a.e + b.e + 50, (lo & (((uint64_t)1 << 50) - 1)) != 0);
}

static dbl d_div(dbl a, dbl b) {                 /* restoring division, 56 quotient bits */
  uint64_t r = a.m, q = 0;
  int i;
  if (!a.m) return a;
  for (i = 0; i < 56; i++) { q <<= 1; if (r >= b.m) { r -= b.m; q |= 1; } r <<= 1; }
  return d_round(q, a.e - b.e - 55, r != 0);
}

/* (float) cast of a double */
static dbl d_f32(dbl a) {
  uint64_t k = a.m >> 29, rem = a.m & ((1u << 29) - 1);
  if (rem > (1u << 28) || (rem == (1u << 28) && (k & 1))) k++;
  if (k == ((uint64_t)1 << 24)) { k >>= 1; a.e++; }
  a.m = k << 29;
  return a;
}

static int d_lt(dbl a, dbl b) {
  if (!a.m) return b.m != 0;
  if (!b.m) return 0;
  return a.e < b.e || (a.e == b.e && a.m < b.m);
}

static int32_t d_floor(dbl a) {                  /* values here are < 2^31 */
  int sh = -a.e;
  if (!a.m || sh >= 64) return 0;
  if (sh <= 0) return 0x7fffffff;
  return (int32_t)(a.m >> sh);
}

static uint32_t d_frac32(dbl a) {                /* fractional part, Q0.32 */
  int sh = -a.e;
  if (!a.m || sh <= 0) return 0;
  if (sh <= 32) return (uint32_t)(a.m << (32 - sh));
  if (sh - 32 >= 64) return 0;
  return (uint32_t)(a.m >> (sh - 32));
}

/* a - n for an integer n <= a; exact because a's quantum divides 1. */
static dbl d_sub_int(dbl a, uint32_t n) {
  if (!n || !a.m) return a;
  return d_round(a.m - ((uint64_t)n << -a.e), a.e, 0);
}

/* ---- float-rounded phase ---------------------------------------------------------- */
static inline int msb32(uint32_t v) {                  /* v != 0 */
#if defined(__ARM_FEATURE_CLZ) || defined(__x86_64__) || defined(__aarch64__)
  return 31 - __builtin_clz(v);
#else
  int p = 0;                                             /* ARM7TDMI, 68000: no clz */
  if (v >> 16) { v >>= 16; p += 16; }
  if (v >> 8) { v >>= 8; p += 8; }
  if (v >> 4) { v >>= 4; p += 4; }
  if (v >> 2) { v >>= 2; p += 2; }
  return p + (int)(v >> 1);
#endif
}

/* Round a positive fixed-point value to 24 significant bits (ties to even), which
 * is what a float holding it would keep. Scale-independent. */
LC_P8SND_HOT static uint64_t fr24(uint64_t x) {
  uint32_t hi = (uint32_t)(x >> 32), lo = (uint32_t)x;
  int p, sh;
  uint64_t rem, half;
  if (hi) p = 32 + msb32(hi);
  else { if (lo < (1u << 24)) return x; p = msb32(lo); }
  sh = p - 23;
  rem = x & (((uint64_t)1 << sh) - 1);
  half = (uint64_t)1 << (sh - 1);
  x -= rem;
  if (rem > half || (rem == half && ((x >> sh) & 1))) x += (uint64_t)1 << sh;
  return x;
}

/* ---- soft float32 --------------------------------------------------------------- */
/* Frequencies are the reference's floats, bit for bit: its note-change detector
 * compares consecutive frequencies against a 1% threshold, and a drop or slide
 * crosses it by margins far below any fixed-point grid. IEEE bits in a u32;
 * normal numbers only (subnormals flush to zero), ties to even. */
typedef uint32_t f32;
#define F_ONE  0x3f800000u
#define F_001  0x3c23d70au      /* 0.01f */
#define F_VIB  0x3f879c7du      /* 1.059463094359f */
#define F_SIGN 0x80000000u
#define F_1000 0x447a0000u      /* 1000.f */

LC_P8SND_HOT static f32 f_mul(f32 a, f32 b) {
  uint32_t sign = (a ^ b) & F_SIGN;
  int32_t ea = (a >> 23) & 255, eb = (b >> 23) & 255, e;
  uint64_t p, m, rem, half;
  int sh = 23;
  if (!ea || !eb) return sign;
  p = (uint64_t)((a & 0x7fffffu) | 0x800000u) * ((b & 0x7fffffu) | 0x800000u);
  e = ea + eb - 127;
  if (p >> 47) { sh = 24; e++; }
  m = p >> sh;
  rem = p & (((uint64_t)1 << sh) - 1);
  half = (uint64_t)1 << (sh - 1);
  if ((rem > half || (rem == half && (m & 1))) && ++m == (1u << 24)) { m >>= 1; e++; }
  if (e <= 0) return sign;
  return sign | ((uint32_t)e << 23) | ((uint32_t)m & 0x7fffffu);
}

LC_P8SND_HOT static f32 f_add(f32 a, f32 b) {
  uint32_t ma, mb, m, sign, low;
  int32_t ea, eb, d;
  if (!(a & 0x7fffffffu)) return b;
  if (!(b & 0x7fffffffu)) return a;
  if ((a & 0x7fffffffu) < (b & 0x7fffffffu)) { f32 t = a; a = b; b = t; }
  sign = a & F_SIGN;
  ea = (a >> 23) & 255; eb = (b >> 23) & 255;
  ma = ((a & 0x7fffffu) | 0x800000u) << 3;          /* 3 guard bits */
  mb = ((b & 0x7fffffu) | 0x800000u) << 3;
  d = ea - eb;
  if (d > 27) mb = 1;
  else if (d) mb = (mb >> d) | ((mb & ((1u << d) - 1)) != 0);
  if ((a ^ b) & F_SIGN) {
    m = ma - mb;
    if (!m) return 0;
    while (m < (1u << 26)) { m <<= 1; ea--; }
  } else {
    m = ma + mb;
    if (m >> 27) { m = (m >> 1) | (m & 1); ea++; }
  }
  low = m & 7;
  m >>= 3;
  if ((low > 4 || (low == 4 && (m & 1))) && ++m == (1u << 24)) { m >>= 1; ea++; }
  if (ea <= 0) return sign;
  return sign | ((uint32_t)ea << 23) | (m & 0x7fffffu);
}

static f32 f_div(f32 a, f32 b) {                     /* positive operands */
  uint32_t mb = (b & 0x7fffffu) | 0x800000u, r = (a & 0x7fffffu) | 0x800000u, q = 0, low;
  int32_t e;
  int i;
  if (!(a & 0x7f800000u)) return 0;
  for (i = 0; i < 26; i++) { q <<= 1; if (r >= mb) { r -= mb; q |= 1; } r <<= 1; }
  e = (int32_t)((a >> 23) & 255) - (int32_t)((b >> 23) & 255) + 126;
  if (q >> 25) { low = ((q & 3) << 1) | (r != 0); q >>= 2; e++; }   /* guard, round, sticky */
  else { low = ((q & 1) << 2) | (r != 0); q >>= 1; }
  if ((low > 4 || (low == 4 && (q & 1))) && ++q == (1u << 24)) { q >>= 1; e++; }
  if (e <= 0) return 0;
  return ((uint32_t)e << 23) | (q & 0x7fffffu);
}

/* v / 2^fb -> float, |v| < 2^24 (exact) */
static f32 f_fix(int32_t v, int fb) {
  uint32_t sign = 0, m;
  int32_t e = 127 + 23 - fb;
  if (!v) return 0;
  if (v < 0) { sign = F_SIGN; m = (uint32_t)-v; } else m = (uint32_t)v;
  while (m < (1u << 23)) { m <<= 1; e--; }
  return sign | ((uint32_t)e << 23) | (m & 0x7fffffu);
}

/* (float) of a dbl */
static f32 f_dbl(dbl a) {
  if (!a.m) return 0;
  a = d_f32(a);
  if (a.e + 179 <= 0) return 0;
  return ((uint32_t)(a.e + 179) << 23) | ((uint32_t)(a.m >> 29) & 0x7fffffu);
}

/* (float)(freq * inv) as a Q32.32 phase step; inv is 1/22050 or 0.5/22050 */
static uint64_t f_step(f32 freq, const dbl *inv) {
  uint64_t mf = (freq & 0x7fffffu) | 0x800000u, ph, pl, x;
  int32_t e;
  dbl p;
  if (!(freq & 0x7f800000u)) return 0;
  ph = mf * (inv->m >> 32);                          /* 77-bit product, >> 20 */
  pl = mf * (uint32_t)inv->m;
  x = (ph << 12) + (pl >> 20);
  p = d_f32(d_round(x, (int32_t)((freq >> 23) & 255) - 150 + inv->e + 20, (pl & 0xfffff) != 0));
  e = p.e + 32;
  if (e >= 0) return p.m << e;
  if (e <= -64) return 0;
  return p.m >> -e;
}

/* ---- tables ------------------------------------------------------------------------ */
/* 440 * 2^((key-33)/12) as the reference's float (glibc exp2f) */
static const f32 key_freq[64] = {
  0x4282d012, 0x428a9760, 0x4292d518, 0x429b9041, 0x42a4d053, 0x42ae9d37, 0x42b8ff49, 0x42c3ff69,
  0x42cfa701, 0x42dc0000, 0x42e914f7, 0x42f6f110, 0x4302d012, 0x430a9761, 0x4312d517, 0x431b9041,
  0x4324d054, 0x432e9d37, 0x4338ff49, 0x4343ff6b, 0x434fa700, 0x435c0000, 0x436914f5, 0x4376f110,
  0x4382d012, 0x438a9761, 0x4392d517, 0x439b9041, 0x43a4d054, 0x43ae9d37, 0x43b8ff49, 0x43c3ff6a,
  0x43cfa700, 0x43dc0000, 0x43e914f7, 0x43f6f110, 0x4402d012, 0x440a9761, 0x4412d517, 0x441b9041,
  0x4424d054, 0x442e9d37, 0x4438ff49, 0x4443ff6a, 0x444fa700, 0x445c0000, 0x446914f7, 0x4476f110,
  0x4482d012, 0x448a9761, 0x4492d517, 0x449b9041, 0x44a4d054, 0x44ae9d37, 0x44b8ff49, 0x44c3ff6b,
  0x44cfa700, 0x44dc0000, 0x44e914f5, 0x44f6f112, 0x4502d012, 0x450a9760, 0x4512d518, 0x451b9041
};

static const int32_t vol_q30[8] = {    /* n / 7.f, exact */
  0, 153391696, 306783392, 460175072, 613566784, 766958464, 920350144, 1073741824
};

/* 1.5 * (1 + (1 - key/63)^2), Q12: noise gain per key */
static const uint16_t noise_gain[64] = {
  12288, 12095, 11904, 11717, 11533, 11351, 11173, 10999, 10827, 10658, 10492, 10330, 10170,
  10014, 9861, 9711, 9564, 9420, 9279, 9141, 9006, 8875, 8746, 8621, 8499, 8379, 8263, 8150,
  8040, 7933, 7830, 7729, 7632, 7537, 7446, 7358, 7272, 7190, 7111, 7036, 6963, 6893, 6827,
  6763, 6703, 6646, 6591, 6540, 6492, 6447, 6406, 6367, 6331, 6299, 6269, 6243, 6220, 6200,
  6183, 6169, 6158, 6150, 6146, 6144
};

/* float thresholds on the phase fraction, Q0.32 (exact float values) */
#define T_0875  3758096384u
#define T_0975  4187593216u
#define T_04    1717986944u
#define T_0255  1095216640u
#define T_0316  1357209728u
#define T_HALF  0x80000000u

/* damp1: highshelf 2400 Hz -6 dB, damp2: highshelf 1000 Hz -12 dB (RBJ cookbook,
 * coefficients c1..c5 as FAKE-08's float filter computes them), Q14 */
static const int16_t damp_c[2][5] = {
  { 9644, -8911, 3151, -19713, 7213 },
  { 4735, -6840, 2691, -28104, 12306 }
};

enum { FX_NONE, FX_SLIDE, FX_VIBRATO, FX_DROP, FX_FADE_IN, FX_FADE_OUT, FX_ARP_FAST, FX_ARP_SLOW };
enum { I_TRI, I_TILTED, I_SAW, I_SQUARE, I_PULSE, I_ORGAN, I_NOISE, I_PHASER };

/* ---- state ------------------------------------------------------------------------- */
typedef struct {
  int16_t sfx;
  int8_t prev_key;
  int8_t last_note;          /* end note of a one-shot sfx, cached per note */
  int16_t ln_at;             /* note index last_note was computed at, -1 stale */
  uint8_t c_speed, c_half;   /* the speed/rate the cached steps belong to */
  int32_t prev_vol;          /* Q30 */
  dbl off, time;
  dbl step;                  /* offset advance per sample */
  dbl ops;                   /* offset_per_second */
  uint32_t kvib;             /* 7.5 / ops, Q4.28 */
  dbl kvd;                   /* 7.5 / ops as a double */
} sstate;

typedef struct {
  uint8_t inst, custom, filters, key, is_music;
  f32 freq;                  /* Hz, float bits */
  int32_t vol;               /* Q30 */
  uint64_t phi, last_adv;    /* Q32.32 */
  int32_t last_smp;          /* Q24 */
  int32_t c_scale, c_rec;    /* noise: 1 / (1 + scale) cached per phase step */
  f32 c_freq;                /* phase step cached per frequency and rate */
  uint8_t c_half;
  uint64_t c_inc;
} synth;

typedef struct {
  sstate main, cust;
  int16_t sfx_music;
  int32_t length;
  uint8_t can_loop, is_music;
  uint8_t last_main_inst, last_main_key;
  synth last, fade_s;
  int32_t fade;              /* Q24 */
  int32_t damp[2][4];        /* x1 x2 y1 y2, Q20 */
  uint8_t damp_live[2];
} chan_t;

static chan_t ch[4];
static int16_t reverb[4][732];     /* Q14 */
static uint16_t reverb_pos;

static struct {
  int16_t count, pattern;
  uint8_t mask;
  uint8_t fade_inf;          /* fade_volume_step == -FLT_MAX */
  f32 fade_f, step_f;        /* fade_volume, fade_volume_step: floats, like the reference */
  f32 inc_f[2];              /* (float)(step * inv_frames_per_second), full and half rate */
  int32_t fade_vol;          /* fade_f as Q30, for the mixer */
  dbl off;                   /* meaningful while pattern != -1 */
  int32_t length;
} mus;

static dbl inv_fps;          /* the reference's 1.0 / 22050.0 */
static dbl music_step;       /* 22050 / 183 * inv_fps */
static uint32_t rng = 0x2545f491u;

uint32_t lc_p8_snd_rand(void);
uint32_t lc_p8_snd_rand(void) {    /* 31-bit xorshift32; also drives the reference in tests */
  rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
  return rng >> 1;
}

/* ---- memory accessors ------------------------------------------------------------ */
static inline uint32_t sfx_addr(int n) { return P8_SFX + (uint32_t)n * 68u; }
static inline uint8_t sfx_speed(int n) { return p8_rd8(sfx_addr(n) + 65); }
static inline uint8_t sfx_lstart(int n) { return p8_rd8(sfx_addr(n) + 66); }
static inline uint8_t sfx_lend(int n) { return p8_rd8(sfx_addr(n) + 67); }
static inline uint8_t sfx_filters(int n) { return p8_rd8(sfx_addr(n) + 64); }
static inline uint16_t sfx_note(int n, int i) {
  uint32_t a = sfx_addr(n) + (uint32_t)i * 2u;
  return (uint16_t)(p8_rd8(a) | (p8_rd8(a + 1) << 8));
}
#define N_KEY(w)    ((w) & 0x3f)
#define N_WAVE(w)   ((((w) >> 6) & 3) | (((w) >> 6) & 4))
#define N_VOL(w)    (((w) >> 9) & 7)
#define N_FX(w)     (((w) >> 12) & 7)
#define N_CUSTOM(w) (((w) >> 15) & 1)
static inline uint8_t song_byte(int p, int i) { return p8_rd8(P8_MUSIC + (uint32_t)p * 4u + (uint32_t)i); }

/* filters byte: bit1 noiz, bit2 buzz, then base-3 digits detune/reverb/dampen */
static const uint8_t filt3[32] = {  /* (q%3) | ((q/3)%3)<<2 | ((q/9)%3)<<4, q = f>>3 */
  0x00, 0x01, 0x02, 0x04, 0x05, 0x06, 0x08, 0x09, 0x0a, 0x10, 0x11, 0x12, 0x14, 0x15, 0x16, 0x18,
  0x19, 0x1a, 0x20, 0x21, 0x22, 0x24, 0x25, 0x26, 0x28, 0x29, 0x2a, 0x00, 0x01, 0x02, 0x04, 0x05
};
#define F_DETUNE(f) (filt3[(f) >> 3] & 3)
#define F_REVERB(f) ((filt3[(f) >> 3] >> 2) & 3)
#define F_DAMPEN(f) ((filt3[(f) >> 3] >> 4) & 3)

static inline int32_t iabs(int32_t v) { return v < 0 ? -v : v; }
static inline int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
/* lerp(a, b, t) = (b - a) * t + a, t Q24 */
static inline int32_t lerp24(int32_t a, int32_t b, int32_t t) { return a + (int32_t)(((int64_t)(b - a) * t) >> 24); }
static inline int32_t mulq30(int32_t a, int32_t k) { return (int32_t)(((int64_t)a * k) >> 30); }

/* ---- waveforms --------------------------------------------------------------------- */
#define ONE24 (1 << 24)

/* phi * f for f = fi + ff/2^32, Q32.32 (low bits of the product truncated) */
static uint64_t phi_mul(uint64_t phi, uint32_t fi, uint32_t ff) {
  uint32_t ip = (uint32_t)(phi >> 32), fp = (uint32_t)phi;
  return phi * fi + (uint64_t)ip * ff + (((uint64_t)fp * ff) >> 32);
}

/* 2t/a - 1 below a, 2(1-t)/(1-a) - 1 above (a = 0.875 or 0.975f), Q24 */
static int32_t tilt(uint32_t t, int32_t T, int wide) {
  if (wide) return t < T_0975 ? (int32_t)(((uint64_t)T * 2202547277u) >> 30) - ONE24
                              : (int32_t)(((uint64_t)(ONE24 - T) * 1342178560u) >> 24) - ONE24;
  return t < T_0875 ? (int32_t)(((uint64_t)T * 2454267026u) >> 30) - ONE24 : (ONE24 - T) * 16 - ONE24;
}

LC_P8SND_HOT static int32_t waveform(synth *p) {       /* Q24 */
  uint32_t t = (uint32_t)p->phi;          /* fmod(phi, 1), Q0.32 */
  int32_t T = (int32_t)(t >> 8);          /* Q24 */
  int buzz = p->filters & 4, noiz = p->filters & 2;
  int32_t r;
  switch (p->inst) {
  case I_TRI:
    r = ONE24 - iabs(4 * T - 2 * ONE24);
    if (buzz) r = (3 * r + tilt(t, T, 0)) >> 2;      /* averaged with a tilted saw */
    return r >> 1;
  case I_TILTED:
    return tilt(t, T, buzz) >> 1;
  case I_SAW:
    r = t < T_HALF ? T : T - ONE24;
    if (buzz) {
      int in = (p->phi >> 32) & 1 ? t < T_HALF : t > T_HALF;   /* |fmod(phi,2) - 1| < 0.5 */
      r = mulq30(r, 891205696) - (in ? 1426063 : 0);           /* * 0.83f - 0.085f */
    }
    return mulq30(r, 701153408);                               /* * 0.653f */
  case I_SQUARE:
    return t < (buzz ? T_04 : T_HALF) ? ONE24 / 4 : -ONE24 / 4;
  case I_PULSE:
    return t < (buzz ? T_0255 : T_0316) ? ONE24 / 4 : -ONE24 / 4;
  case I_ORGAN:
    r = t < T_HALF ? 3 * ONE24 - iabs(24 * T - 6 * ONE24) : ONE24 - iabs(16 * T - 12 * ONE24);
    if (buzz) {                                                /* cut into the first triangle */
      if (t < T_HALF) r = r * 2 + 3 * ONE24;
      r = (t < T_HALF && r > -(ONE24 + ONE24 * 7 / 8)) ? mulq30(r, 214748368) - ONE24 : r + ONE24 / 2;
    }
    return mulq30(r, 119304647);                               /* / 9 */
  case I_NOISE: {
    /* one-pole lowpass of white noise, the pole set by the phase step */
    int64_t da = (int64_t)(p->phi - p->last_adv);
    int32_t scale, rnd, ns;
    if (da > ((int64_t)1 << 42)) da = (int64_t)1 << 42;
    if (da < -((int64_t)1 << 42)) da = -((int64_t)1 << 42);
    scale = (int32_t)((da * 580578) >> 32);                    /* * 8.858923f, Q16 */
    rnd = (int32_t)(lc_p8_snd_rand() >> 6) - ONE24;             /* rand_float(), Q24 */
    if (scale != p->c_scale || !p->c_rec) {
      int32_t den = 65536 + scale;
      if (den == 0) den = 1;
      p->c_scale = scale;
      p->c_rec = (int32_t)((1 << 30) / den);                   /* 1 / (1 + scale), Q14 */
      if (!p->c_rec) p->c_rec = den < 0 ? -1 : 1;
    }
    /* (last + scale*r) / (1 + scale) == r + (last - r) / (1 + scale) */
    ns = rnd + (int32_t)(((int64_t)(p->last_smp - rnd) * p->c_rec) >> 14);
    ns = clampi(ns, -(1 << 29), 1 << 29);
    p->last_adv = p->phi;
    p->last_smp = ns;
    r = (int32_t)(((int64_t)ns * noise_gain[p->key & 63]) >> 12);   /* * 1.5 (1 + f^2) */
    if (noiz) r = (int32_t)(((int64_t)r * (t < T_HALF ? 2 * T : 2 * (T - ONE24))) >> 24);
    return r;
  }
  case I_PHASER: {
    /* two triangles, the second at 109/110 of the frequency */
    uint32_t t2 = (uint32_t)fr24(phi_mul(p->phi, 0, 4255922176u));   /* float product */
    r = 2 * ONE24 - iabs(8 * T - 4 * ONE24);
    r += ONE24 - iabs(4 * (int32_t)(t2 >> 8) - 2 * ONE24);
    if (buzz) {
      r += ONE24 / 4 - iabs((int32_t)((t * 2u + T_HALF) >> 8) - ONE24 / 2);
      r += ONE24 / 8 - iabs((int32_t)((t * 4u) >> 9) - ONE24 / 4);
    }
    return mulq30(r, 178956971);                               /* / 6 */
  }
  }
  return 0;
}

LC_P8SND_HOT static int32_t synth_sample(synth *p) {   /* get_synth_sample: Q24, clamped to +-1 */
  int32_t w = waveform(p), v;
  int detune = F_DETUNE(p->filters);
  if (detune && p->inst != I_NOISE) {
    /* a second wave slightly (or an interval) off */
    synth s2 = *p;
    uint32_t fi, ff;
    if (p->inst == I_TRI) { fi = detune == 1 ? 0 : 1; ff = detune == 1 ? 0xc0000000u : 0x80000000u; }
    else if (p->inst == I_ORGAN) { fi = detune == 1 ? 1 : 4; ff = detune == 1 ? 21582848u : 86331392u; }
    else if (p->inst == I_PHASER) { fi = detune == 1 ? 0 : 2; ff = detune == 1 ? 4209068032u : 43165696u; }
    else { fi = detune == 1 ? 1 : 2; ff = detune == 1 ? 21582848u : 43165696u; }
    s2.phi = fr24(phi_mul(p->phi, fi, ff));                /* phi *= factor, in float */
    if (detune == 2 && p->inst == I_ORGAN) s2.inst = I_TRI;
    w += waveform(&s2) >> 1;
  }
  w = clampi(w, -(ONE24 << 3), ONE24 << 3);
  v = p->vol >> 1;                                        /* volume_music / volume_sfx = 0.5 */
  if (p->is_music) v = mulq30(v, mus.fade_vol);
  return clampi((int32_t)(((int64_t)w * v) >> 30), -ONE24, ONE24);
}

/* (float)(freq * inv_frames_per_second) as Q32.32, cached per synth */
LC_P8SND_HOT static uint64_t phase_step(synth *p, f32 freq, int half) {
  if (p->c_freq != freq || p->c_half != half) {
    dbl inv = inv_fps;
    if (half) inv.e--;
    p->c_freq = freq;
    p->c_half = (uint8_t)half;
    p->c_inc = f_step(freq, &inv);
  }
  return p->c_inc;
}

/* ---- sfx stepping ------------------------------------------------------------------ */
static void cache_steps(sstate *s, int speed, int half) {
  if (s->c_speed == speed && s->c_half == half) return;
  if (s->c_speed != speed) {
    s->ops = d_div(d_int(22050), d_int(183u * (uint32_t)speed));
    s->step = d_mul(s->ops, inv_fps);
    s->kvib = (uint32_t)(((uint64_t)speed * 1069359204u) >> 6);       /* 7.5*183*speed/22050, Q28 */
    s->kvd = d_div(d_int(15), s->ops);
    s->kvd.e--;
  } else if (s->c_half) s->step.e++;
  if (half) s->step.e--;                                   /* 0.5 / 22050 */
  s->c_speed = (uint8_t)speed;
  s->c_half = (uint8_t)half;
}

/* m * 7.5 * offset / offset_per_second, Q32.32, approximate (arpeggio step) */
static uint64_t vib_pos(const sstate *s, dbl off, uint32_t m) {
  uint32_t oq = ((uint32_t)d_floor(off) << 24) | (d_frac32(off) >> 8);   /* Q8.24 */
  return (((uint64_t)oq * s->kvib) >> 20) * m;
}

/* (float)(fabs(fmod(7.5 * offset / offset_per_second, 1.0) - 0.5) - 0.25).
 * The double is formed as offset * (7.5 / ops); only when the float rounding
 * of t is too close to call is the reference expression redone exactly. */
static uint64_t r53(uint64_t u) {                      /* round to a double's 53 bits */
  int p = 63, sh;
  uint64_t rem, half;
  if (u < M53) return u;
  while (!(u >> p)) p--;
  sh = p - 52;
  rem = u & (((uint64_t)1 << sh) - 1);
  half = (uint64_t)1 << (sh - 1);
  u -= rem;
  if (rem > half || (rem == half && ((u >> sh) & 1))) u += (uint64_t)1 << sh;
  return u;
}

static f32 vib_t_of(dbl q, int *close) {
  uint64_t x = 0, y, u, rem, half, k;
  int64_t t;
  uint32_t sign = 0;
  int sh = -q.e, p = 63, fs;
  if (q.m && sh > 0) x = sh <= 64 ? q.m << (64 - sh) : sh < 128 ? q.m >> (sh - 64) : 0;   /* frac, Q0.64 */
  y = r53(x >> 63 ? x - ((uint64_t)1 << 63) : ((uint64_t)1 << 63) - x);   /* fabs(fmod - 0.5) */
  t = (int64_t)y - ((int64_t)1 << 62);                                      /* - 0.25 */
  t = t < 0 ? -(int64_t)r53((uint64_t)-t) : (int64_t)r53((uint64_t)t);
  if (t < 0) { sign = F_SIGN; u = (uint64_t)-t; } else u = (uint64_t)t;
  *close = 1;                                            /* tiny t: always redo exactly */
  if (!u) return 0;
  while (!(u >> p)) p--;
  fs = p - 23;                                           /* bits below the float lsb */
  if (fs <= 0) return sign | ((uint32_t)(p + 63) << 23) | ((uint32_t)(u << -fs) & 0x7fffffu);
  rem = u & (((uint64_t)1 << fs) - 1);
  half = (uint64_t)1 << (fs - 1);
  k = u >> fs;
  /* our double may be an ulp or two off the reference's (2^-44 here) */
  *close = fs <= 24 || (rem > half ? rem - half : half - rem) < ((uint64_t)1 << 24);
  if ((rem > half || (rem == half && (k & 1))) && ++k == (1u << 24)) { k >>= 1; p++; }
  return sign | ((uint32_t)(p + 63) << 23) | ((uint32_t)k & 0x7fffffu);
}

static f32 vib_t(const sstate *s, dbl off) {
  int close;
  f32 t = vib_t_of(d_mul(s->kvd, off), &close);
  if (close) {
    dbl a = d_int(15);
    a.e--;
    t = vib_t_of(d_div(d_mul(a, off), s->ops), &close);
  }
  return t;
}

static void launch(int c, int sfx, dbl offset, int32_t length, int is_music) {
  chan_t *h = &ch[c];
  h->main.sfx = (int16_t)sfx;
  h->main.off = offset;
  h->main.time = d_int(0);
  h->main.ln_at = -1;
  h->length = length < 0 ? 0 : length;
  h->can_loop = 1;
  h->is_music = (uint8_t)is_music;
  h->last_main_inst = 0xff;
  h->last_main_key = 0xff;
  h->main.prev_key = 24;
  h->main.prev_vol = 0;
}

/* update_sfx_state: advance one sample and fill *ns from the current note. */
LC_P8SND_HOT static void update_sfx(sstate *cs, synth *ns, f32 factor, int32_t length, int is_music,
                       int can_loop, int half_rate, int chan_half) {
  int idx, speed, lstart, lend, has_end = 0, note_id, next_id;
  int32_t end_time = 32;
  dbl off, next;
  if (cs->sfx == -1) return;
  idx = cs->sfx;
  speed = sfx_speed(idx);
  if (speed < 1) speed = 1;
  lstart = sfx_lstart(idx);
  lend = sfx_lend(idx);
  cache_steps(cs, speed, chan_half);

  off = cs->off;
  next = off;
  d_step(&next, &cs->step);
  d_step(&cs->time, &cs->step);
  note_id = d_floor(off);
  next_id = d_floor(next);

  if (lend > lstart && can_loop && next_id >= lend) {
    int32_t k = (next_id - lstart) / (lend - lstart);
    next = d_sub_int(next, (uint32_t)(k * (lend - lstart)));
    next_id = d_floor(next);
  }

  if (length > 0) { has_end = 1; end_time = length; }
  if (!is_music && lend == 0 && lstart > 0) { has_end = 1; if (lstart < end_time) end_time = lstart; }
  if (lend <= lstart) {
    has_end = 1;
    if (!is_music) {
      int note = note_id;
      if (cs->ln_at != note) {        /* rescan once per note, not per sample */
        int n, last = 0;
        for (n = 0; n < 32; ++n) if (N_VOL(sfx_note(idx, n))) last = n + 1;
        cs->last_note = (int8_t)last;
        cs->ln_at = (int16_t)note;
      }
      if (cs->last_note < end_time) end_time = cs->last_note;
    }
  }

  if (note_id < 32) {
    uint16_t w = sfx_note(idx, note_id);
    int key = N_KEY(w);
    int32_t vol = vol_q30[N_VOL(w)];
    f32 freq = factor == F_ONE ? key_freq[key] : f_mul(key_freq[key], factor);
    if (vol > 0) {
      uint32_t t32 = d_frac32(off);                          /* fmod(offset, 1) */
      switch (N_FX(w)) {
      case FX_SLIDE: {                                       /* lerp(prev, freq, t) */
        f32 pf = key_freq[cs->prev_key & 63];
        freq = f_add(f_mul(f_add(freq, pf ^ F_SIGN), f_dbl(d_sub_int(off, (uint32_t)note_id))), pf);
        if (cs->prev_vol > 0) vol = cs->prev_vol + (int32_t)(((int64_t)(vol - cs->prev_vol) * (t32 >> 1)) >> 31);
        break;
      }
      case FX_VIBRATO:                                       /* lerp(freq, freq * 1.0594631, t) */
        freq = f_add(f_mul(f_add(f_mul(freq, F_VIB), freq ^ F_SIGN), vib_t(cs, off)), freq);
        break;
      case FX_DROP:                                          /* freq *= 1 - t */
        freq = f_mul(freq, f_add(F_ONE, f_dbl(d_sub_int(off, (uint32_t)note_id)) ^ F_SIGN));
        break;
      /* a fade leaves the volume tiny but nonzero whenever t is: keep it >= 1 so
       * "volume > 0" (which gates the noise generator) agrees with the float */
      case FX_FADE_IN:
        vol = (int32_t)(((int64_t)vol * t32) >> 32);
        if (!vol && off.m && -off.e < 64 && (off.m & (((uint64_t)1 << -off.e) - 1))) vol = 1;
        break;
      case FX_FADE_OUT:                                      /* (float)t rounds to 1 near the end */
        if (t32 >= 0xffffff00u && f_dbl(d_sub_int(off, (uint32_t)note_id)) == F_ONE) vol = 0;
        else {
          vol = (int32_t)(((int64_t)vol * (int64_t)(((uint64_t)1 << 32) - t32)) >> 32);
          if (!vol) vol = 1;
        }
        break;
      case FX_ARP_FAST:
      case FX_ARP_SLOW: {
        uint32_t m = (speed <= 8 ? 32u : 16u) >> (N_FX(w) == FX_ARP_FAST ? 2 : 3);
        uint64_t v = vib_pos(cs, off, m);
        int n = (int)(v >> 32);
        uint32_t fr = (uint32_t)v;
        if (fr < (1u << 20) || fr > 0xffffffffu - (1u << 20)) {
          /* close to a step: evaluate the reference's double expression exactly */
          dbl a = d_int(15u * m);
          a.e--;
          n = d_floor(d_div(d_mul(a, off), cs->ops));
        }
        freq = key_freq[N_KEY(sfx_note(idx, (note_id & ~3) | (n & 3)))];
        break;
      }
      }
      if (half_rate && (freq & 0x7f800000u)) freq -= 1u << 23;   /* * 0.5f */
      ns->key = (uint8_t)key;
      ns->freq = freq;
      ns->inst = (uint8_t)N_WAVE(w);
      ns->custom = (uint8_t)N_CUSTOM(w);
      ns->filters = sfx_filters(idx);
      ns->vol = vol;
      ns->is_music = (uint8_t)is_music;
      /* phi += (float)(freq * inv_frames_per_second), in float */
      ns->phi = fr24(ns->phi + phase_step(ns, freq, chan_half));
    }
    if (next_id != note_id) {
      cs->prev_key = (int8_t)key;
      cs->prev_vol = vol_q30[N_VOL(w)];
    }
  }

  cs->off = next;
  if (has_end && d_floor(cs->time) >= end_time) cs->sfx = -1;
}

/* ---- music ------------------------------------------------------------------------- */
/* float in [0, 1] -> Q30 */
static int32_t f_q30(f32 v) {
  int32_t e = (int32_t)((v >> 23) & 255) - 120;
  uint32_t m = (v & 0x7fffffu) | 0x800000u;
  if (!(v & 0x7f800000u) || (v & F_SIGN)) return 0;
  return (int32_t)(e >= 0 ? m << e : e > -32 ? m >> -e : 0);
}

static void set_fade_step(f32 step) {
  int h;
  mus.step_f = step;
  for (h = 0; h < 2; ++h) {
    dbl sd, inv = inv_fps;
    if (h) inv.e--;
    if (!(step & 0x7f800000u)) { mus.inc_f[h] = step; continue; }
    sd.m = (uint64_t)((step & 0x7fffffu) | 0x800000u) << 29;
    sd.e = (int32_t)((step >> 23) & 255) - 179;
    mus.inc_f[h] = f_dbl(d_mul(sd, inv)) | (step & F_SIGN);
  }
}

static void set_music_pattern(int pattern) {
  int i, n;
  int16_t dur_loop = -1, dur_noloop = -1, dur;
  uint8_t chans[4];
  for (i = 0; i < 4; ++i)
    if (ch[i].is_music) { ch[i].main.sfx = -1; ch[i].sfx_music = -1; }
  if (pattern < 0 || pattern > 63) {
    mus.pattern = -1; mus.count = -1; mus.mask = 0; mus.length = 0;
    mus.off = d_int(0);
    return;
  }
  for (i = 0; i < 4; ++i) chans[i] = song_byte(pattern, i) & 0x7f;
  for (i = 0; i < 4; ++i) {
    int ls, le, sp;
    n = chans[i];
    if (n & 0x40) continue;
    ls = sfx_lstart(n & 0x3f); le = sfx_lend(n & 0x3f); sp = sfx_speed(n & 0x3f);
    if (le > 0 && le > ls) {
      int16_t d = (int16_t)(32 * sp);
      if (d > dur_loop) dur_loop = d;
    } else {
      int16_t end = 32;
      if (le == 0 && ls > 0 && ls < end) end = (int16_t)ls;
      dur_noloop = (int16_t)(end * sp);
      break;
    }
  }
  dur = dur_noloop > 0 ? dur_noloop : dur_loop;
  if (dur <= 0) dur = 32;
  mus.pattern = (int16_t)pattern;
  mus.off = d_int(0);
  mus.length = dur;
  for (i = 0; i < 4; ++i) {
    n = chans[i];
    if (n & 0x40) continue;
    if (ch[i].main.sfx == -1) launch(i, n, d_int(0), 0, 1);
    else ch[i].sfx_music = (int16_t)n;
  }
}

/* ---- API --------------------------------------------------------------------------- */
void lc_p8_audio_init(void) {
  int i, j;
  for (i = 0; i < 4; ++i) {
    uint8_t *b = (uint8_t *)&ch[i];
    for (j = 0; j < (int)sizeof(chan_t); ++j) b[j] = 0;
    for (j = 0; j < 732; ++j) reverb[i][j] = 0;
    ch[i].main.sfx = -1;
    ch[i].main.prev_key = 24;
    ch[i].cust.sfx = -1;
    ch[i].sfx_music = -1;
    ch[i].can_loop = 1;
    ch[i].main.ln_at = ch[i].cust.ln_at = -1;
  }
  reverb_pos = 0;
  mus.count = -1; mus.pattern = -1; mus.mask = 0;
  mus.fade_vol = 0; mus.fade_f = 0; mus.fade_inf = 0;
  mus.step_f = 0; mus.inc_f[0] = mus.inc_f[1] = 0;
  mus.off = d_int(0); mus.length = 0;
  inv_fps = d_div(d_int(1), d_int(22050));
  music_step = d_mul(d_div(d_int(22050), d_int(183)), inv_fps);
  rng = 0x2545f491u;
}

/* sfx(n, [channel], [offset], [length]): the caller passes FAKE-08's defaults
 * for missing arguments: channel -1, offset 0, length -1 (n truncated to int).
 * Returns nothing; FAKE-08's api_sfx result is the chosen channel. */
void lc_p8_sfx(int n, int channel, int offset, int length) {
  int i;
  if (n < -2 || n > 63 || channel < -2 || channel > 3 || offset > 31) return;
  if (channel == -2) {
    for (i = 0; i < 4; ++i) if (ch[i].main.sfx == n) ch[i].main.sfx = -1;
    return;
  }
  if (n == -1 || n == -2) {
    for (i = 0; i < 4; ++i) {
      if (channel != -1 && i != channel) continue;
      if (ch[i].is_music) continue;
      if (n == -1) ch[i].main.sfx = -1; else ch[i].can_loop = 0;
    }
    return;
  }
  if (channel == -1)
    for (i = 0; i < 4; ++i) {
      if ((1 << i) & mus.mask) continue;
      if (ch[i].main.sfx == -1 || ch[i].main.sfx == n) { channel = i; break; }
    }
  if (channel == -1)
    for (i = 0; i < 4; ++i) {
      if ((1 << i) & mus.mask) continue;
      if (ch[i].is_music) { channel = i; break; }
    }
  if (channel == -1) {
    int fastest = 255;
    for (i = 0; i < 4; ++i) {
      int idx = ch[i].main.sfx;
      if ((1 << i) & mus.mask) continue;
      if (idx < 0 || idx >= 64) continue;
      if (sfx_speed(idx) <= fastest) { channel = i; fastest = sfx_speed(idx); }
    }
  }
  if (channel == -1) return;
  for (i = 0; i < 4; ++i) if (ch[i].main.sfx == n) ch[i].main.sfx = -1;
  if (ch[channel].main.sfx != -1 && ch[channel].is_music) ch[channel].sfx_music = ch[channel].main.sfx;
  launch(channel, n, d_int((uint32_t)(offset < 0 ? 0 : offset)), length < 0 ? 0 : length, 0);
}

/* music(n, [fade_ms], [mask]): defaults fade 0, mask 0. FAKE-08 narrows
 * fade and mask to int16. */
void lc_p8_music(int n, int fade_ms, int mask) {
  int16_t fade = (int16_t)fade_ms, m = (int16_t)mask;
  if (n < -1 || n > 63) return;
  if (n == -1) {
    /* music stops once the fade-out is done */
    if (fade <= 0) mus.fade_inf = 1;
    else { mus.fade_inf = 0; set_fade_step(f_mul(mus.fade_f, f_div(F_1000, f_fix(fade, 0))) ^ F_SIGN); }
    return;
  }
  mus.count = 0;
  mus.mask = (uint8_t)(m & 0xf);
  mus.fade_inf = 0;
  mus.fade_f = F_ONE;
  set_fade_step(0);
  if (fade > 0) {
    mus.fade_f = 0;
    set_fade_step(f_div(F_1000, f_fix(fade, 0)));
  }
  mus.fade_vol = f_q30(mus.fade_f);
  set_music_pattern(n);
}

int lc_p8_audio_stat(int n) {
  if (n >= 46 && n <= 56) n -= 30;
  if (n >= 16 && n <= 19) return ch[n - 16].main.sfx;
  if (n >= 20 && n <= 23) return ch[n - 20].main.sfx < 0 ? -1 : d_floor(ch[n - 20].main.off);
  if (n == 24) return mus.pattern;
  if (n == 25) return mus.count;
  if (n == 26) return mus.pattern == -1 ? -1 : (int16_t)d_floor(mus.off);
  return 0;
}

/* ---- the mixer --------------------------------------------------------------------- */
/* y = c1 x + c2 x1 + c3 x2 - c4 y1 - c5 y2; Q20 state split into hi/lo bytes so
 * every product is 16x16 */
#define BQ_HI(v) ((int16_t)((v) >> 8))
#define BQ_LO(v) ((int16_t)((v) & 255))
LC_P8SND_HOT static int32_t biquad(int32_t *s, const int16_t *c, int32_t x24) {
  int32_t x = clampi(x24, -(4 << 24), (4 << 24) - 1) >> 4, y, ah, al;   /* Q20, |x| < 4 */
  ah = c[0] * BQ_HI(x) + c[1] * BQ_HI(s[0]) + c[2] * BQ_HI(s[1]) - c[3] * BQ_HI(s[2]) - c[4] * BQ_HI(s[3]);
  al = c[0] * BQ_LO(x) + c[1] * BQ_LO(s[0]) + c[2] * BQ_LO(s[1]) - c[3] * BQ_LO(s[2]) - c[4] * BQ_LO(s[3]);
  y = clampi((ah >> 6) + ((al + 8192) >> 14), -(4 << 20), (4 << 20) - 1);
  s[1] = s[0]; s[0] = x;
  s[3] = s[2]; s[2] = y;
  return y << 4;
}

/* Prime a damping filter from the last 96 inputs (the reverb ring holds them,
 * current sample excluded); 96 samples settle both filters below -80 dB. */
#define DAMP_PRIME 96
static void damp_prime(chan_t *h, int c, int k) {
  int32_t tmp[4] = { 0, 0, 0, 0 }, *st = h->damp[k];
  int i, p = reverb_pos - DAMP_PRIME;
  if (p < 0) p += 732;
  for (i = 0; i < 4; ++i) st[i] = 0;
  for (i = 0; i < DAMP_PRIME; ++i) {
    int32_t v = reverb[c][p] << 10;
    if (k == 1 && h->damp_live[0]) v = biquad(tmp, damp_c[0], v);   /* damp2 fed by damp1 */
    biquad(st, damp_c[k], v);
    if (++p == 732) p = 0;
  }
}

LC_P8SND_HOT void lc_p8_audio_fill(int16_t *out, int n) {
  int i, c;
  uint8_t half_reg, rev_reg, dist_reg, lp_reg;
  for (i = 0; i < n; ++i) {
    int32_t mix = 0;
    int is_pause = p8_rd8(P8_SNDPAUSE) == 1;
    half_reg = p8_rd8(P8_HALFRATE);
    rev_reg = p8_rd8(P8_REVERB);
    dist_reg = p8_rd8(P8_DISTORT);
    lp_reg = p8_rd8(P8_LOWPASS);
    for (c = 0; c < 4; ++c) {
      chan_t *h = &ch[c];
      int chalf = (half_reg >> c) & 1;
      synth ns;
      int32_t value = 0, r1, r2, d1, d2;
      int16_t smp;
      int16_t *rv = reverb[c];

      if (c == 0 && mus.pattern != -1 && !is_pause) {
        dbl st = music_step;
        if (chalf) st.e--;
        d_step(&mus.off, &st);
        if (mus.fade_inf) mus.fade_f = 0;
        else if (mus.inc_f[chalf]) {
          f32 fv = f_add(mus.fade_f, mus.inc_f[chalf]);
          mus.fade_f = (fv & F_SIGN) ? 0 : fv > F_ONE ? F_ONE : fv;   /* clamp(0, 1) */
        }
        mus.fade_vol = f_q30(mus.fade_f);
        if ((mus.fade_inf || ((mus.step_f & F_SIGN) && (mus.step_f & 0x7fffffffu))) && !mus.fade_f)
          set_music_pattern(-1);
        else if (d_floor(mus.off) >= mus.length) {
          int next = mus.pattern + 1, count = mus.count + 1;
          if (song_byte(mus.pattern, 2) & 0x80) { next = -1; count = -1; }
          else if (song_byte(mus.pattern, 1) & 0x80)
            while (--next > 0 && !(song_byte(next, 0) & 0x80)) ;
          mus.count = (int16_t)count;
          set_music_pattern(next);
        }
      }

      if (h->main.sfx == -1 && h->sfx_music != -1 && !is_pause) {
        /* resume the music sfx where the song is now (music.offset / speed) */
        int idx = h->sfx_music, sp = sfx_speed(idx), ls = sfx_lstart(idx), le = sfx_lend(idx);
        if (sp < 1) sp = 1;
        if (mus.pattern == -1) launch(c, idx, d_int(0), 0, 1);   /* offset -1: clamps to 0 */
        else {
          dbl no = d_div(mus.off, d_int((uint32_t)sp));
          int32_t fl = d_floor(no);
          int over = d_frac32(no) != 0;
          if (le > ls && h->can_loop) {
            if (fl > ls || (fl == ls && over)) no = d_sub_int(no, (uint32_t)((fl - ls) / (le - ls) * (le - ls)));
            launch(c, idx, d_f32(no), 0, 1);
          } else if (!(fl > 32 || (fl == 32 && over))) launch(c, idx, d_f32(no), 0, 1);
        }
        h->sfx_music = -1;
      }

      /* a silent channel with nothing pending: all FAKE-08 would do is write a
       * zero into the reverb ring (and run the damp filters, primed on demand) */
      if (h->main.sfx == -1 && (h->sfx_music == -1 || is_pause) && h->fade <= 0 &&
          !h->last.vol && !h->last.freq && !h->last.inst && !((rev_reg | lp_reg) & (0x11 << c))) {
        h->last.custom = h->last.filters = h->last.key = h->last.is_music = 0;
        if (!is_pause) h->last_main_inst = h->last_main_key = 0;
        rv[reverb_pos] = 0;
        h->damp_live[0] = h->damp_live[1] = 0;
        continue;
      }

      ns.inst = 0; ns.custom = 0; ns.filters = 0; ns.key = 0; ns.is_music = 0;
      ns.freq = 0; ns.vol = 0;
      ns.c_freq = h->last.c_freq; ns.c_half = h->last.c_half; ns.c_inc = h->last.c_inc;
      ns.c_scale = h->last.c_scale; ns.c_rec = h->last.c_rec;
      ns.phi = h->last.phi;
      ns.last_adv = h->last.last_adv;
      ns.last_smp = h->last.last_smp;

      if (!is_pause) {
        dbl base = h->main.off;
        int hr = (half_reg >> (c + 4)) & 1, restart;
        update_sfx(&h->main, &ns, F_ONE, h->length, h->is_music, h->can_loop, hr, chalf);
        restart = ns.inst != h->last_main_inst || ns.key != h->last_main_key;
        h->last_main_inst = ns.inst;
        h->last_main_key = ns.key;
        if (ns.vol > 0) {
          if (ns.custom) {
            int32_t mvol;
            f32 factor;
            if (d_lt(h->main.off, base)) restart = 1;
            if (h->cust.sfx == -1 && d_floor(base) != d_floor(h->main.off)) restart = 1;
            if (restart) {
              h->cust.sfx = ns.inst;
              h->cust.off = d_int(0);
              h->cust.time = d_int(0);
              h->cust.ln_at = -1;
            }
            ns.phi = h->last.phi;
            factor = f_div(ns.freq, key_freq[24]);             /* freq / key_to_freq(24) */
            mvol = ns.vol;
            update_sfx(&h->cust, &ns, factor, 0, 0, 1, hr, chalf);
            ns.vol = (int32_t)(((int64_t)ns.vol * mvol) >> 30);
            if (!ns.vol) ns.vol = 1;                           /* both factors are > 0 */
          }
          value = synth_sample(&ns);
        }
      }

      /* harsh state changes start a short crossfade from the previous synth */
      {
        /* abs(new.freq - last.freq) > min(new.freq, last.freq) * 0.01f, in float */
        f32 fmin = ns.freq < h->last.freq ? ns.freq : h->last.freq;
        int fjump = ns.freq != h->last.freq &&
                    (f_add(ns.freq, h->last.freq ^ F_SIGN) & 0x7fffffffu) > f_mul(fmin, F_001);
        if (iabs(ns.vol - h->last.vol) > 107374182 || fjump || ns.inst != h->last.inst) {
          if (h->fade <= 0) h->fade_s = h->last;
          h->fade = 1 << 24;
          ns.phi &= 0xffffffffu;                 /* fmod(phi, 1) */
        }
      }
      h->last = ns;

      /* filter weights, Q24 */
      r1 = F_REVERB(ns.filters) == 1 ? ONE24 : 0;
      r2 = F_REVERB(ns.filters) == 2 ? ONE24 : 0;
      d1 = F_DAMPEN(ns.filters) == 1 ? ONE24 : 0;
      d2 = F_DAMPEN(ns.filters) == 2 ? ONE24 : 0;

      if (h->fade > 0) {
        int32_t vf, fw = h->fade;
        synth *fs = &h->fade_s;
        int fr = F_REVERB(fs->filters), fdp = F_DAMPEN(fs->filters);
        fs->phi = fr24(fs->phi + phase_step(fs, fs->freq, chalf));
        vf = synth_sample(fs);
        value = lerp24(value, vf, fw);
        r1 = lerp24(r1, fr == 1 ? ONE24 : 0, fw);
        r2 = lerp24(r2, fr == 2 ? ONE24 : 0, fw);
        d1 = lerp24(d1, fdp == 1 ? ONE24 : 0, fw);
        d2 = lerp24(d2, fdp == 2 ? ONE24 : 0, fw);
        h->fade -= chalf ? 49457 : 98913;        /* 130 / 22050 per sample, Q24 */
      }

      if (rev_reg & (1 << (c + 4))) r1 = ONE24;
      if (rev_reg & (1 << c)) r2 = ONE24;
      if (lp_reg & (1 << (c + 4))) d1 = ONE24;
      if (lp_reg & (1 << c)) d2 = ONE24;

      {
        int p366 = reverb_pos >= 366 ? reverb_pos - 366 : reverb_pos + 366;
        if (r1 > 0) value += (int32_t)(((int64_t)r1 * rv[p366]) >> 15);        /* * 0.5; ring is Q14 */
        if (r2 > 0) value += (int32_t)(((int64_t)r2 * rv[reverb_pos]) >> 15);
        rv[reverb_pos] = (int16_t)clampi(value >> 10, -32768, 32767);
      }

      if (d1 > 0) {
        if (!h->damp_live[0]) damp_prime(h, c, 0);
        value = lerp24(value, biquad(h->damp[0], damp_c[0], value), d1);
      }
      if (d2 > 0) {
        if (!h->damp_live[1]) damp_prime(h, c, 1);
        value = lerp24(value, biquad(h->damp[1], damp_c[1], value), d2);
      }
      h->damp_live[0] = d1 > 0;
      h->damp_live[1] = d2 > 0;

      {   /* (int16_t)(32767.99f * clamp(value, -0.99f, 0.99f)): float product, truncated */
        int32_t v = clampi(value, -16609444, 16609444);
        uint64_t pr = (uint64_t)(v < 0 ? -v : v) * 33554422u;          /* < 2^49: ulp <= 2^26 */
        int32_t m = (int32_t)((((uint32_t)(pr >> 26) & 0xff) != 0xff ? pr : fr24(pr)) >> 34);
        smp = (int16_t)(v < 0 ? -m : m);
      }
      if (dist_reg & (1 << c)) smp = (int16_t)(smp / 0x1000 * 0x1249);
      else if (dist_reg & (1 << (c + 4))) smp = (int16_t)((smp - (smp < 0 ? 0x1000 : 0)) / 0x1000 * 0x1249);
      mix += smp;
    }
    reverb_pos = reverb_pos == 731 ? 0 : reverb_pos + 1;
    out[i] = (int16_t)clampi(mix, -32767, 32767);
  }
}
