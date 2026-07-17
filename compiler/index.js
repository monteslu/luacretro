// luacretro compiler entry - source text in, C text (or diagnostics) out.
//
// The shared Lua front-end for the gtlua / gbalua / mdlua console SDKs. Each
// SDK calls compile() with its target + its builtin tables (passed via opts),
// so this package stays platform-agnostic and byte-identical to the SDKs'
// previous in-tree front-ends.

import { lex } from "./lexer.js";
import { parse } from "./parser.js";
import { check } from "./check.js";
import { emit } from "./emit.js";

/**
 * @typedef {{file:string,line:number,col:number,severity:"error"|"warning",message:string}} Diagnostic
 */

/**
 * Compile PICO-8-flavored Lua to C for a console target.
 * @param {string} source
 * @param {string} file  name used in diagnostics
 * @param {object} [opts]
 *   - target: "gametank" | "gba" | "md"  (selects codegen)
 *   - sdkName: "gtlua" | "gbalua" | "mdlua"  (diagnostic + generated-by text)
 *   - builtins, members, callbacks: the SDK's merged tables
 *   - p8Palette, nearestColorByte: GameTank color tooling (gametank only)
 *   - banked, placement, num8, inliner: GameTank build knobs (passthrough)
 * @returns {{ok, c, diagnostics, callGraph?, stubs?}}
 */
export function compile(source, file = "main.lua", opts = {}) {
  const sdkName = opts.sdkName;
  const { tokens, diagnostics: lexDiags } = lex(source, file);
  const { chunk, diagnostics: parseDiags } = parse(tokens, file, sdkName);
  const diagnostics = [...lexDiags, ...parseDiags];

  // Don't typecheck a broken parse - the errors would be noise.
  if (diagnostics.some((d) => d.severity === "error")) {
    return { ok: false, c: null, diagnostics };
  }

  const { diagnostics: checkDiags, symbols } = check(chunk, file, opts);
  diagnostics.push(...checkDiags);
  if (diagnostics.some((d) => d.severity === "error")) {
    return { ok: false, c: null, diagnostics };
  }

  const out = emit(chunk, symbols, file, opts);
  return { ok: true, c: out.c, diagnostics, callGraph: out.callGraph, stubs: out.stubs };
}

/** Render diagnostics the way compilers do: file:line:col: severity: message */
export function formatDiagnostics(diagnostics) {
  return diagnostics
    .map((d) => `${d.file}:${d.line}:${d.col}: ${d.severity}: ${d.message}`)
    .join("\n");
}
