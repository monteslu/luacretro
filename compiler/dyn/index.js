// luacretro dynamic tier - entry point: PICO-8 cart or Lua source in, C out.
//
// The generated C links against the portable runtime in luacretro/runtime
// (value/heap/table/coroutine core plus the PICO-8 machine) and an SDK's
// platform layer (runtime/lc_plat.h). RUNTIME_SOURCES lists the runtime
// translation units an SDK build must compile.

import { lexFull } from "./lex.js";
import { parseFull } from "./parse.js";
import { emitDynamic } from "./emit.js";
import { loadCart, ROM_SIZE } from "./cart.js";
import { unicodeToP8scii } from "./p8scii.js";

export const RUNTIME_SOURCES = [
  "lc_gc.c", "lc_str.c", "lc_tab.c", "lc_vm.c", "lc_co.c", "lc_lib.c", "lc_libc.c",
  "lc_p8gfx.c", "lc_p8print.c", "lc_p8snd.c", "lc_p8sys.c", "lc_p8lib.c",
];
export const RUNTIME_HEADERS = [
  "lc.h", "lc_int.h", "lc_lib.h", "lc_plat.h", "lc_p8.h", "lc_cart.h", "lc_trig.h",
  "lc_p8snd_fast.c", // #included by lc_p8snd.c when built with -DLC_P8SND_FAST
];

/** unicode text (from a .p8 or a .lua file) -> P8SCII byte string */
export function toP8scii(text) {
  const bytes = unicodeToP8scii(text);
  let s = "";
  for (let i = 0; i < bytes.length; i += 8192) s += String.fromCharCode(...bytes.subarray(i, i + 8192));
  return s;
}

/**
 * Compile Lua (P8SCII byte string) with an optional cart ROM image.
 * @param {string} p8src
 * @param {{file?: string, rom?: Uint8Array}} [opts]
 * @returns {{ok: boolean, c: string|null, diagnostics: object[]}}
 */
export function compileLua(p8src, opts = {}) {
  const file = opts.file ?? "main.lua";
  const { tokens, diagnostics: ld } = lexFull(p8src, file);
  if (ld.length) return { ok: false, c: null, diagnostics: ld };
  const { chunk, diagnostics: pd } = parseFull(tokens, file);
  if (!chunk) return { ok: false, c: null, diagnostics: pd };
  const { c, diagnostics } = emitDynamic(chunk, { file, rom: opts.rom, debugLines: !!opts.debugLines });
  return { ok: !!c, c, diagnostics: [...pd, ...diagnostics] };
}

/**
 * Compile a cart file (.p8 / .p8.png bytes, or a plain .lua text file).
 * @param {Uint8Array} bytes
 * @param {string} name
 * @param {{resolveInclude?: Function}} [opts]
 */
export function compileCart(bytes, name, opts = {}) {
  if (/\.lua$/i.test(name)) {
    const text = new TextDecoder().decode(bytes);
    return { ...compileLua(toP8scii(text), { file: name, rom: new Uint8Array(ROM_SIZE), debugLines: opts.debugLines }), cart: null };
  }
  const cart = loadCart(bytes, name, opts);
  const src = cart.luaEncoding === "unicode" ? toP8scii(cart.lua) : cart.lua;
  return { ...compileLua(src, { file: name, rom: cart.rom, debugLines: opts.debugLines }), cart };
}
