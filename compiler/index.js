// gtlua compiler entry - source text in, C text (or diagnostics) out.

import { lex } from "./lexer.js";
import { parse } from "./parser.js";
import { check } from "./check.js";
import { emit } from "./emit.js";

/**
 * @typedef {{file:string,line:number,col:number,severity:"error"|"warning",message:string}} Diagnostic
 */

/**
 * Compile gtlua source to C.
 * @param {string} source
 * @param {string} file name used in diagnostics
 * @param {object} [opts] - {banked:true, placement:{fnName:"fixed"|"b0"|"b1"|"b2"}}
 *   enables the FLASH2M banked build: functions land in per-bank segments and
 *   cross-bank calls are routed through generated far-call stubs.
 * @returns {{ok: boolean, c: string|null, diagnostics: Diagnostic[],
 *            callGraph?: Map<string,Set<string>>, stubs?: string|null}}
 */
export function compile(source, file = "main.lua", opts = {}) {
  const { tokens, diagnostics: lexDiags } = lex(source, file);
  const { chunk, diagnostics: parseDiags } = parse(tokens, file);
  const diagnostics = [...lexDiags, ...parseDiags];

  // Don't typecheck a broken parse - the errors would be noise.
  if (diagnostics.some((d) => d.severity === "error")) {
    return { ok: false, c: null, diagnostics };
  }

  const { diagnostics: checkDiags, symbols } = check(chunk, file);
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
