// luacretro dynamic tier - the PICO-8 API as seen by the compiler.
//
// Every name here is a builtin closure in the runtime (lc_lib.c /
// lc_p8lib.c): `sym` is its C name. A global that the cart never assigns
// keeps its builtin value for the whole run, so calls to it compile to a
// direct C call; `fast` lists the argument counts for which the compiler
// can also skip the value stack and call a typed runtime entry point.

/** name -> runtime symbol (lcb_<sym>) */
export const BUILTINS = {
  // base library + PICO-8 math/string/table helpers (lc_lib.c)
  max: "max", min: "min", mid: "mid", flr: "flr", ceil: "ceil", sin: "sin", cos: "cos",
  atan2: "atan2", sqrt: "sqrt", abs: "abs", sgn: "sgn", band: "band", bor: "bor",
  bxor: "bxor", bnot: "bnot", shl: "shl", shr: "shr", lshr: "lshr", rotl: "rotl",
  rotr: "rotr", tostr: "tostr", tonum: "tonum", chr: "chr", ord: "ord", sub: "sub",
  split: "split", type: "type", tostring: "tostring", next: "next", pairs: "pairs",
  inext: "inext", ipairs: "ipairs", select: "select", rawget: "rawget", rawset: "rawset",
  rawequal: "rawequal", rawlen: "rawlen", setmetatable: "setmetatable",
  getmetatable: "getmetatable", unpack: "unpack", pack: "pack", assert: "assert",
  cocreate: "cocreate", coresume: "coresume", costatus: "costatus", yield: "yield",
  all: "all", foreach: "foreach", count: "count", add: "add", del: "del", deli: "deli",
  // PICO-8 machine (lc_p8lib.c)
  cls: "cls", pset: "pset", pget: "pget", color: "color", line: "line", tline: "tline",
  circ: "circ", circfill: "circfill", oval: "oval", ovalfill: "ovalfill", rect: "rect",
  rectfill: "rectfill", rrect: "rrect", rrectfill: "rrectfill", print: "print",
  cursor: "cursor", spr: "spr", sspr: "sspr", fget: "fget", fset: "fset", sget: "sget",
  sset: "sset", camera: "camera", clip: "clip", mget: "mget", mset: "mset", map: "map",
  mapdraw: "map", pal: "pal", palt: "palt", fillp: "fillp", btn: "btn", btnp: "btnp",
  time: "time", t: "time", stat: "stat", music: "music", sfx: "sfx", cstore: "cstore",
  memcpy: "memcpy", memset: "memset", peek: "peek", poke: "poke", peek2: "peek2",
  poke2: "poke2", peek4: "peek4", poke4: "poke4", reload: "reload", reset: "reset",
  cartdata: "cartdata", dget: "dget", dset: "dset", printh: "printh", rnd: "rnd",
  srand: "srand", flip: "flip", run: "run", extcmd: "extcmd", stop: "stop",
  menuitem: "menuitem", serial: "serial", _set_fps: "noop", trace: "noop", save: "noop",
  info: "noop", abort: "noop", folder: "noop", resume: "noop", reboot: "noop", dir: "noop",
  ls: "noop", load: "noop",
};

/**
 * API names PICO-8 binds as locals of the chunk that wraps the cart ("save
 * host cpu by making most of the api local", zep): they resolve lexically,
 * so they stay reachable when a cart replaces _ENV. rrect/rrectfill are
 * newer and not in the list (observed in PICO-8 0.2.7).
 */
export const API_LOCALS = new Set(("time t sub chr ord tostr tonum add del deli clip color pal palt " +
  "fillp pget pset sget sset fget fset circ circfill rect rectfill oval ovalfill line spr sspr " +
  "mget mset tline peek poke peek2 poke2 peek4 poke4 memcpy memset max min mid flr ceil cos sin " +
  "atan2 rnd srand band bor bxor bnot shl shr lshr rotl rotr").split(" "));

/** P8SCII glyph variables the PICO-8 BIOS predefines: byte -> 16.16 bits */
export const GLYPH_GLOBALS = {
  "\x8b": 0 << 16, "\x91": 1 << 16, "\x94": 2 << 16, "\x83": 3 << 16, "\x8e": 4 << 16, "\x97": 5 << 16,
  "\x80": 0, "\x81": 0x5a5a8000, "\x82": 0x511f8000, "\x84": 0x7d7d8000, "\x85": 0xb81d8000 | 0,
  "\x86": 0xf99f8000 | 0, "\x87": 0x51bf8000, "\x88": 0xb5bf8000 | 0, "\x89": 0x999f8000 | 0,
  "\x8a": 0xb11f8000 | 0, "\x8c": 0xa0e08000 | 0, "\x8d": 0x9b3f8000 | 0, "\x8f": 0xb1bf8000 | 0,
  "\x90": 0xf5ff8000 | 0, "\x92": 0xb15f8000 | 0, "\x93": 0x1b1f8000, "\x95": 0xf5bf8000 | 0,
  "\x96": 0x7adf8000, "\x98": 0x0f0f8000, "\x99": 0x55558000,
};

/*
 * Typed fast paths. Each entry: argument count -> { ret, c(args) } where
 * args are { n(): C expr of the argument as int32 16.16 (lua_tonumber
 * semantics), i(): as a floored int, v(): as an lv } and ret is the C type
 * class of the result ("num" int32 bits, "bool" C int, "void", "lv").
 * These must reproduce the builtin's behaviour for exactly that argument
 * count (the runtime builtins pick overloads by count).
 */
export const FAST = {
  flr: { 1: { ret: "num", c: (a) => `(int32_t)((uint32_t)(${a[0].n()}) & 0xffff0000u)` } },
  ceil: { 1: { ret: "num", c: (a) => `lc_ceil(${a[0].n()})` } },
  abs: { 1: { ret: "num", c: (a) => `lc_absn(${a[0].n()})` } },
  sgn: { 1: { ret: "num", c: (a) => `((${a[0].n()}) >= 0 ? 0x10000 : -0x10000)` } },
  min: { 2: { ret: "num", c: (a) => `lc_minn(${a[0].n()}, ${a[1].n()})` } },
  max: { 2: { ret: "num", c: (a) => `lc_maxn(${a[0].n()}, ${a[1].n()})` } },
  mid: { 3: { ret: "num", c: (a) => `lc_midn(${a[0].n()}, ${a[1].n()}, ${a[2].n()})` } },
  sin: { 1: { ret: "num", c: (a) => `lc_sin_bits(${a[0].n()})` } },
  cos: { 1: { ret: "num", c: (a) => `lc_sin_bits((int32_t)((uint32_t)(${a[0].n()}) - 0x4000u))` } },
  atan2: { 2: { ret: "num", c: (a) => `lc_atan2_bits(${a[0].n()}, ${a[1].n()})` } },
  sqrt: { 1: { ret: "num", c: (a) => `lc_sqrt_bits(${a[0].n()})` } },
  band: { 2: { ret: "num", c: (a) => `((${a[0].n()}) & (${a[1].n()}))` } },
  bor: { 2: { ret: "num", c: (a) => `((${a[0].n()}) | (${a[1].n()}))` } },
  bxor: { 2: { ret: "num", c: (a) => `((${a[0].n()}) ^ (${a[1].n()}))` } },
  bnot: { 1: { ret: "num", c: (a) => `(~(${a[0].n()}))` } },
  shl: { 2: { ret: "num", c: (a) => `lc_fshl(${a[0].n()}, ${a[1].n()})` } },
  shr: { 2: { ret: "num", c: (a) => `lc_fshr(${a[0].n()}, ${a[1].n()})` } },
  lshr: { 2: { ret: "num", c: (a) => `lc_flshr(${a[0].n()}, ${a[1].n()})` } },
  rotl: { 2: { ret: "num", c: (a) => `lc_frotl(${a[0].n()}, ${a[1].n()})` } },
  rotr: { 2: { ret: "num", c: (a) => `lc_frotr(${a[0].n()}, ${a[1].n()})` } },
  btn: {
    1: { ret: "bool", c: (a) => `lc_p8_btn(${a[0].i()}, 0)` },
    2: { ret: "bool", c: (a) => `lc_p8_btn(${a[0].i()}, ${a[1].i()})` },
  },
  btnp: {
    1: { ret: "bool", c: (a) => `lc_p8_btnp(${a[0].i()}, 0)` },
    2: { ret: "bool", c: (a) => `lc_p8_btnp(${a[0].i()}, ${a[1].i()})` },
  },
  pget: { 2: { ret: "num", c: (a) => `(lc_p8_pget(${a[0].i()}, ${a[1].i()}) << 16)` } },
  sget: { 2: { ret: "num", c: (a) => `(lc_p8_sget(${a[0].i()}, ${a[1].i()}) << 16)` } },
  mget: { 2: { ret: "num", c: (a) => `(lc_p8_mget(${a[0].i()}, ${a[1].i()}) << 16)` } },
  fget: {
    1: { ret: "num", c: (a) => `(lc_p8_fget((${a[0].i()}) & 0xff) << 16)` },
    2: { ret: "bool", c: (a) => `((lc_p8_fget((${a[0].i()}) & 0xff) >> ((${a[1].i()}) & 0xff)) & 1)` },
  },
  pset: {
    2: { ret: "void", c: (a) => `lc_p8_pset(${a[0].i()}, ${a[1].i()}, -1)` },
    3: { ret: "void", c: (a) => `lc_p8_pset(${a[0].i()}, ${a[1].i()}, (${a[2].i()}) & 0xff)` },
  },
  rectfill: {
    4: { ret: "void", c: (a) => `lc_p8_rectfill(${a[0].i()}, ${a[1].i()}, ${a[2].i()}, ${a[3].i()}, lc_p8mem[P8_COLOR])` },
    5: { ret: "void", c: (a) => `lc_p8_rectfill(${a[0].i()}, ${a[1].i()}, ${a[2].i()}, ${a[3].i()}, (uint8_t)(int16_t)(${a[4].i()}))` },
  },
  rect: {
    4: { ret: "void", c: (a) => `lc_p8_rect(${a[0].i()}, ${a[1].i()}, ${a[2].i()}, ${a[3].i()}, lc_p8mem[P8_COLOR])` },
    5: { ret: "void", c: (a) => `lc_p8_rect(${a[0].i()}, ${a[1].i()}, ${a[2].i()}, ${a[3].i()}, (uint8_t)(int16_t)(${a[4].i()}))` },
  },
  line: {
    4: { ret: "void", c: (a) => `lc_p8_line(${a[0].i()}, ${a[1].i()}, ${a[2].i()}, ${a[3].i()}, lc_p8mem[P8_COLOR])` },
    5: { ret: "void", c: (a) => `lc_p8_line(${a[0].i()}, ${a[1].i()}, ${a[2].i()}, ${a[3].i()}, (uint8_t)(int16_t)(${a[4].i()}))` },
  },
  circfill: {
    3: { ret: "void", c: (a) => `lc_p8_circfill(${a[0].i()}, ${a[1].i()}, ${a[2].i()}, lc_p8mem[P8_COLOR])` },
    4: { ret: "void", c: (a) => `lc_p8_circfill(${a[0].i()}, ${a[1].i()}, ${a[2].i()}, (uint8_t)(int16_t)(${a[3].i()}))` },
  },
  circ: {
    3: { ret: "void", c: (a) => `lc_p8_circ(${a[0].i()}, ${a[1].i()}, ${a[2].i()}, lc_p8mem[P8_COLOR])` },
    4: { ret: "void", c: (a) => `lc_p8_circ(${a[0].i()}, ${a[1].i()}, ${a[2].i()}, (uint8_t)(int16_t)(${a[3].i()}))` },
  },
  spr: {
    3: { ret: "void", c: (a) => `lc_p8_spr(${a[0].i()}, ${a[1].i()}, ${a[2].i()}, 0x10000, 0x10000, 0, 0)` },
  },
  cls: {
    0: { ret: "void", c: () => `lc_p8_cls(0)` },
    1: { ret: "void", c: (a) => `lc_p8_cls(${a[0].i()})` },
  },
};
