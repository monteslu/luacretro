// luacretro dynamic tier - scope resolution, type inference and C emission.
//
// Every Lua function becomes a C function with the runtime's calling
// convention (arguments on the value stack, results written back over
// them). Values are 64-bit lv; the static tier lives inside this emitter as
// type inference: a local that is only ever assigned numbers (and is not
// captured by a closure) becomes a plain int32_t, and arithmetic,
// comparisons and PICO-8 builtins on such values compile to straight C with
// no tag checks. Everything else goes through the runtime's inline fast
// paths (lc.h) and slow paths (metamethods, strings, errors).
//
// Closed-world facts the emitter exploits (a PICO-8 cart has no load()):
//  - globals live in a static slot array unless the cart touches _ENV as a
//    value, in which case they live in a real table;
//  - a builtin name the cart never assigns is a direct C call;
//  - locals of the main chunk that are not inside a loop are static slots,
//    so top-level functions that use them need no upvalues or heap.

import { BUILTINS, GLYPH_GLOBALS, FAST, API_LOCALS } from "./builtins.js";

const SEED = 0x5eed1234;

/** Lua 5.2 luaS_hash with the runtime's seed (LC_HASH_SEED) */
export function strHash(s) {
  const l = s.length;
  let h = (SEED ^ l) >>> 0;
  const step = (l >>> 5) + 1;
  for (let l1 = l; l1 >= step; l1 -= step) {
    h = (h ^ (((h << 5) >>> 0) + (h >>> 2) + (s.charCodeAt(l1 - 1) & 0xff))) >>> 0;
  }
  return h >>> 0;
}

function int2fb(x) {
  let e = 0;
  if (x < 8) return x;
  while (x >= 0x10) { x = (x + 1) >> 1; e++; }
  return ((e + 1) << 3) | (x - 8);
}
function fb2int(x) {
  const e = (x >> 3) & 0x1f;
  if (e === 0) return x;
  return ((x & 7) + 8) << (e - 1);
}
const luaSizeHint = (n) => fb2int(int2fb(n));

function cstr(s) {
  let out = '"';
  for (let i = 0; i < s.length; i++) {
    const c = s.charCodeAt(i) & 0xff;
    if (c >= 0x20 && c < 0x7f && c !== 0x22 && c !== 0x5c && c !== 0x3f) out += String.fromCharCode(c);
    else out += "\\" + c.toString(8).padStart(3, "0");
  }
  return out + '"';
}

const hex32 = (n) => "0x" + ((n >>> 0).toString(16)) + "u";
const numLit = (bits) => `(int32_t)${hex32(bits)}`;

// ---- constant folding (z8lua fix32 semantics) --------------------------------------

function fmul(a, b) { return Number((BigInt(a) * BigInt(b)) >> 16n) | 0; }
function fdiv(a, b) {
  if (b === 0x10000) return a;
  if (b) {
    const r = (BigInt(a) * 65536n) / BigInt(b);
    const ar = r < 0n ? -r : r;
    if (ar <= 0x7fffffffn) return Number(r) | 0;
  }
  return (a ^ b) >= 0 ? 0x7fffffff : 0x80000001 | 0;
}
function fmod(a, b) {
  b = b >= 0 ? b : ((b << 1) === 0 ? ~b : -b);
  const r = b ? a % b : 0;
  return (r >= 0 ? r : r + b) | 0;
}
function foldBin(op, a, b) {
  switch (op) {
    case "+": return (a + b) | 0;
    case "-": return (a - b) | 0;
    case "*": return fmul(a, b);
    case "/": return fdiv(a, b);
    case "%": return fmod(a, b);
    case "\\": return fdiv(a, b) & 0xffff0000 | 0;
    case "&": return a & b;
    case "|": return a | b;
    case "^^": return a ^ b;
    default: return null;
  }
}

// ---- resolution ------------------------------------------------------------------------

class Var {
  constructor(name, fn, id, depth) {
    this.name = name; this.fn = fn; this.id = id; this.loopDepth = depth;
    this.captured = false; this.isStatic = false; this.staticIdx = -1;
    this.assigns = []; this.num = false; this.funcInit = null; this.reassigned = false;
  }
}

class Fn {
  constructor(node, parent, id) {
    this.node = node; this.parent = parent; this.id = id;
    this.upvals = []; this.upIndex = new Map(); this.children = [];
  }
  upvalFor(v) {
    if (this.upIndex.has(v)) return this.upIndex.get(v);
    const i = this.upvals.length;
    this.upvals.push(v);
    this.upIndex.set(v, i);
    return i;
  }
}

function resolve(chunk, diags, file) {
  const fns = [];
  const globals = new Map();   // name -> { reads, writes, funcDefs: [{fnNode, topLevel}] }
  let envAsValue = false;
  let varId = 0;
  const apiVars = new Map();
  const apiVar = (name) => {
    if (!apiVars.has(name)) {
      const v = new Var(name, null, varId++, 0);
      v.isStatic = true;
      v.isApi = true;
      apiVars.set(name, v);
    }
    return apiVars.get(name);
  };
  const gl = (name) => {
    if (!globals.has(name)) globals.set(name, { name, reads: 0, writes: 0, funcDefs: [] });
    return globals.get(name);
  };

  function newFn(node, parent) {
    const f = new Fn(node, parent, fns.length);
    fns.push(f);
    if (parent) parent.children.push(f);
    node.fn = f;
    return f;
  }

  function lookup(scope, name) {
    for (let s = scope; s; s = s.parent) {
      const v = s.vars.get(name);
      if (v) return v;
    }
    return null;
  }

  function refVar(v, scope) {
    const cur = scope.fn;
    if (v.fn !== cur && !v.isStatic) {
      v.captured = true;
      for (let f = cur; f && f !== v.fn; f = f.parent) f.upvalFor(v);
    }
  }

  function declare(scope, name) {
    const v = new Var(name, scope.fn, varId++, scope.loopDepth);
    scope.vars.set(name, v);
    // main-chunk locals outside loops execute once: static slots
    if (scope.fn.node.isMain && scope.loopDepth === 0 && !scope.fn.node.hasLabels) v.isStatic = true;
    return v;
  }

  const child = (scope, loop = 0) => ({ vars: new Map(), parent: scope, fn: scope.fn, loopDepth: scope.loopDepth + loop });

  function name(e, scope, isWrite) {
    const v = lookup(scope, e.name);
    if (v) {
      e.ref = { kind: "local", v };
      refVar(v, scope);
      return;
    }
    if (API_LOCALS.has(e.name)) {
      const v = apiVar(e.name);
      v.fn = fns[0];
      e.ref = { kind: "local", v };
      return;
    }
    const env = lookup(scope, "_ENV");
    if (env) {
      e.ref = { kind: "env", v: env };
      refVar(env, scope);
      return;
    }
    if (e.name === "_ENV") { envAsValue = true; e.ref = { kind: "genv" }; return; }
    const g = gl(e.name);
    if (isWrite) g.writes++; else g.reads++;
    e.ref = { kind: "global", g };
  }

  function expr(e, scope) {
    switch (e.kind) {
      case "Name": name(e, scope, false); break;
      case "Index": expr(e.obj, scope); expr(e.key, scope); break;
      case "Call": expr(e.fn, scope); e.args.forEach((a) => expr(a, scope)); break;
      case "MethodCall": expr(e.obj, scope); e.args.forEach((a) => expr(a, scope)); break;
      case "Binop": expr(e.left, scope); expr(e.right, scope); break;
      case "Unop": expr(e.expr, scope); break;
      case "Paren": expr(e.expr, scope); break;
      case "Table":
        for (const it of e.items) {
          if (it.kind === "keyed") expr(it.key, scope);
          expr(it.value, scope);
        }
        break;
      case "Function": func(e, scope); break;
      case "Vararg":
        if (!scope.fn.node.vararg) diags.push({ file, line: e.line, col: e.col, severity: "error", message: "cannot use '...' outside a vararg function" });
        break;
      default: break;
    }
  }

  function func(node, scope) {
    const f = newFn(node, scope ? scope.fn : null);
    node.hasLabels = node.isMain && node.body.stmts.some((s) => s.kind === "Label");
    const fs = { vars: new Map(), parent: scope, fn: f, loopDepth: 0 };
    node.paramVars = node.params.map((p) => {
      const v = declare(fs, p);
      v.isStatic = false;
      v.assigns.push("param");
      return v;
    });
    block(node.body, fs);
    return f;
  }

  function assignTarget(t, scope, valueNode) {
    if (t.kind === "Name") {
      name(t, scope, true);
      if (t.ref.kind === "local") {
        t.ref.v.assigns.push(valueNode ?? "unknown");
        t.ref.v.reassigned = true;
      }
    } else { expr(t.obj, scope); expr(t.key, scope); }
  }

  function block(b, scope) {
    for (const s of b.stmts) stmt(s, scope);
  }

  function stmt(s, scope) {
    switch (s.kind) {
      case "Local": {
        s.exprs.forEach((e) => expr(e, scope));
        s.vars = s.names.map((n, i) => {
          const v = declare(scope, n);
          const last = s.exprs.length - 1;
          if (i < s.exprs.length) {
            const e = s.exprs[i];
            if (i === last && s.names.length > s.exprs.length && (e.kind === "Call" || e.kind === "MethodCall" || e.kind === "Vararg")) v.assigns.push("unknown");
            else v.assigns.push(e);
          } else if (last >= 0 && (s.exprs[last].kind === "Call" || s.exprs[last].kind === "MethodCall" || s.exprs[last].kind === "Vararg")) {
            v.assigns.push("unknown");
          } else v.assigns.push("nil");
          return v;
        });
        break;
      }
      case "LocalFunction": {
        const v = declare(scope, s.name);
        s.var = v;
        v.assigns.push(s.func);
        v.funcInit = s.func;
        func(s.func, scope);
        break;
      }
      case "Assign": {
        s.exprs.forEach((e) => expr(e, scope));
        s.targets.forEach((t, i) => {
          const e = s.exprs[i];
          const multi = i >= s.exprs.length - 1 && s.exprs.length && ["Call", "MethodCall", "Vararg"].includes(s.exprs[s.exprs.length - 1].kind) && s.targets.length > s.exprs.length;
          assignTarget(t, scope, multi ? "unknown" : (e ?? "nil"));
          if (t.kind === "Name" && t.ref.kind === "global" && e && e.kind === "Function") {
            t.ref.g.funcDefs.push({ fnNode: e, topLevel: scope.fn.node.isMain && scope.loopDepth === 0 && scope.parent === null });
          }
        });
        break;
      }
      case "CompoundAssign": {
        expr(s.value, scope);
        if (s.target.kind === "Name") {
          name(s.target, scope, true);
          if (s.target.ref.kind === "global") s.target.ref.g.reads++;
          if (s.target.ref.kind === "local") {
            s.target.ref.v.assigns.push({ kind: "Binop", op: s.op, left: s.target, right: s.value, line: s.line, col: s.col });
            s.target.ref.v.reassigned = true;
          }
        } else { expr(s.target.obj, scope); expr(s.target.key, scope); }
        break;
      }
      case "CallStat": expr(s.call, scope); break;
      case "Do": block(s.body, child(scope)); break;
      case "While": expr(s.cond, scope); block(s.body, child(scope, 1)); break;
      case "Repeat": {
        const inner = child(scope, 1);
        block(s.body, inner);
        expr(s.cond, inner);
        break;
      }
      case "If":
        for (const cl of s.clauses) { expr(cl.cond, scope); block(cl.body, child(scope)); }
        if (s.orelse) block(s.orelse, child(scope));
        break;
      case "NumericFor": {
        expr(s.start, scope); expr(s.limit, scope); if (s.step) expr(s.step, scope);
        const inner = child(scope, 1);
        s.var = declare(inner, s.name);
        s.var.isStatic = false;
        s.var.assigns.push("fornum");
        block(s.body, inner);
        break;
      }
      case "GenericFor": {
        s.exprs.forEach((e) => expr(e, scope));
        const inner = child(scope, 1);
        s.vars = s.names.map((n) => {
          const v = declare(inner, n);
          v.isStatic = false;
          v.assigns.push("unknown");
          return v;
        });
        block(s.body, inner);
        break;
      }
      case "Return": s.exprs.forEach((e) => expr(e, scope)); break;
      default: break;
    }
  }

  func(chunk, null);
  return { fns, globals, envAsValue, apiVars };
}

// ---- inference -------------------------------------------------------------------------

function infer(res) {
  const allVars = new Set();
  for (const f of res.fns) {
    const walkVars = (node) => {
      if (!node || typeof node !== "object") return;
      if (Array.isArray(node)) { node.forEach(walkVars); return; }
      if (node.paramVars) node.paramVars.forEach((v) => allVars.add(v));
      if (node.vars && Array.isArray(node.vars)) node.vars.forEach((v) => allVars.add(v));
      if (node.var) allVars.add(node.var);
      for (const k of Object.keys(node)) {
        if (k === "ref" || k === "fn" || k === "paramVars" || k === "vars" || k === "var") continue;
        const c = node[k];
        if (c && typeof c === "object") walkVars(c);
      }
    };
    walkVars(f.node.body);
  }
  const cand = [...allVars].filter((v) => !v.captured && !v.isStatic);
  for (const v of cand) v.num = true;
  let changed = true;
  while (changed) {
    changed = false;
    for (const v of cand) {
      if (!v.num) continue;
      const ok = v.assigns.every((a) => a === "fornum" || (typeof a === "object" && exprIsNum(a, res)));
      if (!ok) { v.num = false; changed = true; }
    }
  }
}

const NUM_BUILTIN_RET = new Set(["flr", "ceil", "abs", "sgn", "sin", "cos", "atan2", "sqrt",
  "min", "max", "mid", "band", "bor", "bxor", "bnot", "shl", "shr", "lshr", "rotl", "rotr",
  "pget", "sget", "mget", "time", "t", "peek2", "peek4"]);

function isPureBuiltin(e, res) {
  if (e.kind !== "Name" || !e.ref) return false;
  if (e.ref.kind === "local") return !!e.ref.v.isApi && !e.ref.v.reassigned;
  return e.ref.kind === "global" && !res.envAsValue &&
    e.ref.g.writes === 0 && Object.prototype.hasOwnProperty.call(BUILTINS, e.name);
}

function exprIsNum(e, res) {
  switch (e.kind) {
    case "Number": return true;
    case "Paren": return exprIsNum(e.expr, res);
    case "Name": return e.ref && e.ref.kind === "local" && e.ref.v.num;
    case "Unop":
      if (e.op === "-" || e.op === "~") return exprIsNum(e.expr, res);
      if (e.op === "@" || e.op === "%" || e.op === "$") return true;
      return false;
    case "Binop":
      if (["+", "-", "*", "/", "%", "\\", "^", "&", "|", "^^", "<<", ">>", ">>>", "<<>", ">><"].includes(e.op))
        return exprIsNum(e.left, res) && exprIsNum(e.right, res);
      return false;
    case "Call": {
      if (!isPureBuiltin(e.fn, res)) return false;
      const n = e.fn.name;
      if (e.args.length && ["Call", "MethodCall", "Vararg"].includes(e.args[e.args.length - 1].kind)) return false;
      if (NUM_BUILTIN_RET.has(n)) return true;
      if (n === "rnd") return e.args.length === 0 || (e.args.length === 1 && exprIsNum(e.args[0], res));
      if (n === "fget") return e.args.length === 1;
      if (n === "peek") return true;
      return false;
    }
    default: return false;
  }
}

// ---- emission ----------------------------------------------------------------------------

export function emitDynamic(chunk, opts) {
  const file = opts.file ?? "main.lua";
  const diags = [];
  const res = resolve(chunk, diags, file);
  if (diags.some((d) => d.severity === "error")) return { c: null, diagnostics: diags };
  infer(res);

  const tableMode = res.envAsValue;
  const strings = new Map();
  const lit = (s) => {
    if (!strings.has(s)) strings.set(s, `s_${strings.size}`);
    return `(const lc_str *)&${strings.get(s)}`;
  };
  // global slots
  const gslot = new Map();
  for (const g of res.globals.values()) gslot.set(g.name, gslot.size);
  for (const cb of ["_init", "_update", "_update60", "_draw"]) if (!gslot.has(cb)) gslot.set(cb, gslot.size);
  let nstatic = 0;
  const fnOut = [];
  const protos = [];
  let tmp = 0;
  const T = (p = "t") => `${p}${tmp++}`;

  // direct-call targets: globals assigned exactly once, at top level, with an
  // upvalue-free function
  const directGlobal = new Map();
  if (!tableMode) {
    for (const g of res.globals.values()) {
      if (g.writes === 1 && g.funcDefs.length === 1 && g.funcDefs[0].topLevel && g.funcDefs[0].fnNode.fn.upvals.length === 0)
        directGlobal.set(g.name, g.funcDefs[0].fnNode.fn);
    }
  }

  const fname = (f) => `f_${f.id}`;
  const fclo = (f) => `fc_${f.id}`;

  // ---- per-function emission ------------------------------------------------------------

  function emitFunction(f) {
    const node = f.node;
    const lines = [];
    let indent = 1;
    const out = (s) => lines.push("  ".repeat(indent) + s);
    const labelStack = [];      // [{ name -> cname }]
    const loopStack = [];       // break targets: null = C loop (break works)

    // ---- variables
    const vdecl = (v) => {
      if (v.isStatic) { if (v.staticIdx < 0) v.staticIdx = nstatic++; return null; }
      return v.captured ? `lc_cell *c${v.id}` : v.num ? `int32_t n${v.id}` : `lv l${v.id}`;
    };
    const vread = (v) => {
      if (v.isStatic) { if (v.staticIdx < 0) v.staticIdx = nstatic++; return { c: `S[${v.staticIdx}]`, t: "lv" }; }
      if (v.fn !== f) return { c: `C->up[${f.upIndex.get(v)}]->v`, t: "lv" };
      if (v.captured) return { c: `c${v.id}->v`, t: "lv" };
      return v.num ? { c: `n${v.id}`, t: "num" } : { c: `l${v.id}`, t: "lv" };
    };
    const vlvalue = (v) => {
      const r = vread(v);
      return r;
    };
    // declare + initialise a local from an expression result
    const declInit = (v, r) => {
      if (v.isStatic) { const x = vread(v); out(`${x.c} = ${toLv(r)};`); return; }
      if (v.captured) { out(`lc_cell *c${v.id} = lc_cell_new(${toLv(r)});`); return; }
      if (v.num) { out(`int32_t n${v.id} = ${toNum(r)};`); return; }
      out(`lv l${v.id} = ${toLv(r)};`);
    };
    const assignVar = (v, r) => {
      const x = vlvalue(v);
      if (x.t === "num") out(`${x.c} = ${toNum(r)};`);
      else out(`${x.c} = ${toLv(r)};`);
    };

    // ---- conversions
    function toLv(r) {
      if (r.t === "lv") return r.c;
      if (r.t === "num") return `LV_NUM(${r.c})`;
      if (r.t === "bool") return `LV_BOOL(${r.c})`;
      if (r.t === "void") return `((void)(${r.c}), LV_NIL)`;
      throw new Error("bad result type " + r.t);
    }
    function toNum(r) {
      if (r.t === "num") return r.c;
      if (r.t === "lv") return `lc_n(${r.c})`;
      return `((void)(${r.c}), 0)`;
    }
    function toCond(r) {
      if (r.t === "bool") return r.c;
      if (r.t === "num") return `((void)(${r.c}), 1)`;
      if (r.t === "void") return `((void)(${r.c}), 0)`;
      return `LV_TRUTHY(${r.c})`;
    }

    const isMulti = (e) => e.kind === "Call" || e.kind === "MethodCall" || e.kind === "Vararg";
    const hasCall = (e) => {
      if (!e || typeof e !== "object") return false;
      if (e.kind === "Call" || e.kind === "MethodCall") return true;
      if (e.kind === "Function") return false;
      if (e.kind === "Table") return e.items.some((it) => hasCall(it.value) || (it.key && hasCall(it.key)));
      for (const k of ["left", "right", "expr", "obj", "key"]) if (e[k] && hasCall(e[k])) return true;
      return false;
    };

    // ---- expressions: return {c, t}
    function ex(e) {
      switch (e.kind) {
        case "Nil": return { c: "LV_NIL", t: "lv" };
        case "True": return { c: "1", t: "bool" };
        case "False": return { c: "0", t: "bool" };
        case "Number": return { c: numLit(e.bits), t: "num" };
        case "String": return { c: `LV_STR(${lit(e.value)})`, t: "lv" };
        case "Vararg": return { c: "(NVA > 0 ? VA[0] : LV_NIL)", t: "lv" };
        case "Paren": {
          const r = ex(e.expr);
          return r;
        }
        case "Name": return nameRead(e);
        case "Index": return indexRead(e);
        case "Call": case "MethodCall": return call(e, "one");
        case "Function": return closure(e.fn);
        case "Table": return table(e);
        case "Unop": return unop(e);
        case "Binop": return binop(e);
        default: throw new Error("unknown expression " + e.kind);
      }
    }

    function nameRead(e) {
      const ref = e.ref;
      if (ref.kind === "local") return vread(ref.v);
      if (ref.kind === "env") return { c: `lc_fld(${toLv(vread(ref.v))}, ${lit(e.name)})`, t: "lv" };
      if (ref.kind === "genv") return { c: "GENV", t: "lv" };
      if (tableMode) return { c: `lc_fld(GENV, ${lit(e.name)})`, t: "lv" };
      return { c: `G[${gslot.get(e.name)}]`, t: "lv" };
    }

    function indexRead(e) {
      const o = ex(e.obj);
      if (e.key.kind === "String") return { c: `lc_fld(${toLv(o)}, ${lit(e.key.value)})`, t: "lv" };
      const k = ex(e.key);
      if (hasCall(e.key)) {
        const to = T();
        return { c: `({ lv ${to} = ${toLv(o)}; lc_idx(${to}, ${toLv(k)}); })`, t: "lv" };
      }
      return { c: `lc_idx(${toLv(o)}, ${toLv(k)})`, t: "lv" };
    }

    // evaluate a list of expressions onto the stack at base var `b`, starting at
    // slot `from`. Returns { code: [...stmts], count: C expr of total count }.
    function pushArgs(args, b, from = 0) {
      const code = [];
      let k = from;
      for (let i = 0; i < args.length; i++) {
        const a = args[i];
        const last = i === args.length - 1;
        if (last && isMulti(a)) {
          if (a.kind === "Vararg") {
            code.push(`lc_movev(${b} + ${k}, VA, NVA); lc_top = ${b} + ${k} + NVA;`);
            return { code, count: `(${k} + NVA)` };
          }
          const m = T("m");
          code.push(`int ${m} = ${callMulti(a)};`);
          code.push(`lc_top = ${b} + ${k} + ${m};`);
          return { code, count: `(${k} + ${m})` };
        }
        code.push(`${b}[${k}] = ${toLv(ex(a))}; lc_top = ${b} + ${k + 1};`);
        k++;
      }
      return { code, count: `${k}` };
    }

    // call target -> how to invoke: returns { pre:[stmts], invoke:(b,n)=>C expr (int results) }
    function callTarget(e) {
      if (e.kind === "MethodCall") {
        const o = T("o");
        return {
          pre: [`lv ${o} = ${toLv(ex(e.obj))};`],
          selfv: o,
          invoke: (b, n) => `lc_call(lc_fld(${o}, ${lit(e.name)}), ${b}, ${n})`,
        };
      }
      const fnE = e.fn;
      if (isPureBuiltin(fnE, res)) {
        const sym = BUILTINS[fnE.name];
        return { pre: [], invoke: (b, n) => `lcb_${sym}(${b}, ${n}, 0)` };
      }
      if (fnE.kind === "Name" && fnE.ref.kind === "global" && !tableMode) {
        if (isPureBuiltin(fnE, res)) {
          const sym = BUILTINS[fnE.name];
          return { pre: [], invoke: (b, n) => `lcb_${sym}(${b}, ${n}, 0)` };
        }
        const df = directGlobal.get(fnE.name);
        if (df) return { pre: [], invoke: (b, n) => `${fname(df)}(${b}, ${n}, (lc_clo *)&${fclo(df)})` };
      }
      if (fnE.kind === "Name" && fnE.ref.kind === "local" && !fnE.ref.v.reassigned && fnE.ref.v.funcInit) {
        const tf = fnE.ref.v.funcInit.fn;
        if (tf.upvals.length === 0) return { pre: [], invoke: (b, n) => `${fname(tf)}(${b}, ${n}, (lc_clo *)&${fclo(tf)})` };
        const r = vread(fnE.ref.v);
        return { pre: [], invoke: (b, n) => `${fname(tf)}(${b}, ${n}, (lc_clo *)LV_PTR(${r.c}))` };
      }
      const fv = T("f");
      return { pre: [`lv ${fv} = ${toLv(ex(fnE))};`], invoke: (b, n) => `lc_call(${fv}, ${b}, ${n})` };
    }

    function fastBuiltin(e) {
      if (e.kind !== "Call" || !isPureBuiltin(e.fn, res)) return null;
      const spec = FAST[e.fn.name];
      if (!spec) return null;
      const n = e.args.length;
      if (!spec[n]) return null;
      if (n && isMulti(e.args[n - 1])) return null;
      // evaluate arguments in order: temporaries when a later one has a call
      const pre = [];
      const vals = e.args.map((a) => ex(a));
      const seq = e.args.some((a, i) => i > 0 && hasCall(a));
      const argObjs = vals.map((r, i) => {
        let rr = r;
        if (seq) {
          const t = T();
          if (r.t === "num") pre.push(`int32_t ${t} = ${r.c};`);
          else pre.push(`lv ${t} = ${toLv(r)};`);
          rr = { c: t, t: r.t === "num" ? "num" : "lv" };
        }
        return { n: () => toNum(rr), i: () => `(${toNum(rr)} >> 16)`, v: () => toLv(rr) };
      });
      const body = spec[n].c(argObjs);
      const ret = spec[n].ret;
      if (!pre.length) return { c: body, t: ret };
      if (ret === "void") return { c: `({ ${pre.join(" ")} ${body}; })`, t: "void" };
      return { c: `({ ${pre.join(" ")} ${body}; })`, t: ret };
    }

    // single-result call expression
    function call(e, mode) {
      const fast = fastBuiltin(e);
      if (fast) return fast;
      const b = T("b");
      const tg = callTarget(e);
      const parts = [`lv *${b} = lc_top;`, ...tg.pre];
      let from = 0;
      if (tg.selfv) { parts.push(`${b}[0] = ${tg.selfv}; lc_top = ${b} + 1;`); from = 1; }
      const pa = pushArgs(e.args, b, from);
      parts.push(...pa.code);
      const n = T("n");
      parts.push(`int ${n} = ${tg.invoke(b, pa.count)};`);
      if (mode === "stmt") {
        parts.push(`(void)${n}; lc_top = ${b};`);
        return { c: `({ ${parts.join(" ")} })`, t: "void" };
      }
      const r = T("r");
      parts.push(`lv ${r} = ${n} > 0 ? ${b}[0] : LV_NIL; lc_top = ${b}; ${r};`);
      return { c: `({ ${parts.join(" ")} })`, t: "lv" };
    }

    // multi-result call: results land at lc_top (the caller has set lc_top to
    // where they should go); evaluates to the result count, lc_top left there
    function callMulti(e) {
      const fast = fastBuiltin(e);
      if (fast) {
        const b = T("b");
        if (fast.t === "void") return `({ ${fast.c}; 0; })`;
        return `({ lv *${b} = lc_top; ${b}[0] = ${toLv(fast)}; 1; })`;
      }
      const b = T("b");
      const tg = callTarget(e);
      const parts = [`lv *${b} = lc_top;`, ...tg.pre];
      let from = 0;
      if (tg.selfv) { parts.push(`${b}[0] = ${tg.selfv}; lc_top = ${b} + 1;`); from = 1; }
      const pa = pushArgs(e.args, b, from);
      parts.push(...pa.code);
      const n = T("n");
      parts.push(`int ${n} = ${tg.invoke(b, pa.count)}; lc_top = ${b}; ${n};`);
      return `({ ${parts.join(" ")} })`;
    }

    function closure(cf) {
      if (cf.upvals.length === 0) return { c: `LV_FUN(&${fclo(cf)})`, t: "lv" };
      const k = T("k");
      const parts = [`lc_clo *${k} = lc_clo_new(${fname(cf)}, ${cf.upvals.length}, ${cstr(cf.node.name ?? "?")});`];
      cf.upvals.forEach((v, i) => {
        const src = v.fn === f ? `c${v.id}` : `C->up[${f.upIndex.get(v)}]`;
        parts.push(`${k}->up[${i}] = ${src};`);
      });
      parts.push(`LV_FUN(${k});`);
      return { c: `({ ${parts.join(" ")} })`, t: "lv" };
    }

    function table(e) {
      const items = e.items;
      const pos = items.filter((it) => it.kind === "pos");
      const lastMulti = pos.length && items[items.length - 1].kind === "pos" && isMulti(items[items.length - 1].value);
      const na = pos.length - (lastMulti ? 1 : 0);
      const nh = items.length - pos.length;
      if (items.length === 0) return { c: "LV_TAB(lc_tab_new(0, 0))", t: "lv" };
      const tb = T("tb"), t = T("tt");
      const parts = [`lv *${tb} = lc_top; lc_tab *${t} = lc_tab_new(${luaSizeHint(na)}, ${luaSizeHint(nh)});`,
        `${tb}[0] = LV_TAB(${t}); lc_top = ${tb} + 1;`];
      let pending = 0, flushed = 0;
      const flush = () => {
        if (!pending) return;
        parts.push(`lc_tab_setlist(${t}, ${flushed + 1}, ${tb} + 1, ${pending}); lc_top = ${tb} + 1;`);
        flushed += pending;
        pending = 0;
      };
      items.forEach((it, idx) => {
        if (it.kind === "named") {
          parts.push(`{ lv v_ = ${toLv(ex(it.value))}; lc_tab_set(${t}, LV_STR(${lit(it.name)}), v_); }`);
        } else if (it.kind === "keyed") {
          const kk = T("kk");
          parts.push(`{ lv ${kk} = ${toLv(ex(it.key))}; ${tb}[1 + ${pending}] = ${kk}; lc_top = ${tb} + 2 + ${pending}; lv v_ = ${toLv(ex(it.value))}; if (${kk} == LV_NIL) lc_error("table index is nil"); lc_tab_set(${t}, ${kk}, v_); lc_top = ${tb} + 1 + ${pending}; }`);
        } else {
          const last = idx === items.length - 1;
          if (last && isMulti(it.value)) {
            if (it.value.kind === "Vararg") {
              parts.push(`lc_movev(${tb} + 1 + ${pending}, VA, NVA); lc_tab_setlist(${t}, ${flushed + 1}, ${tb} + 1, ${pending} + NVA); lc_top = ${tb} + 1;`);
            } else {
              const m = T("m");
              parts.push(`lc_top = ${tb} + 1 + ${pending}; int ${m} = ${callMulti(it.value)}; lc_tab_setlist(${t}, ${flushed + 1}, ${tb} + 1, ${pending} + ${m}); lc_top = ${tb} + 1;`);
            }
            pending = 0;
            return;
          }
          parts.push(`${tb}[1 + ${pending}] = ${toLv(ex(it.value))}; lc_top = ${tb} + ${2 + pending};`);
          pending++;
          if (pending === 50) flush();
        }
      });
      flush();
      parts.push(`lc_top = ${tb}; LV_TAB(${t});`);
      return { c: `({ ${parts.join(" ")} })`, t: "lv" };
    }

    function unop(e) {
      if (e.op === "-" && e.expr.kind === "Number") return { c: numLit(-e.expr.bits | 0), t: "num" };
      const r = ex(e.expr);
      switch (e.op) {
        case "not": return { c: `(!${toCond(r)})`, t: "bool" };
        case "-": return r.t === "num" ? { c: `(int32_t)(0u - (uint32_t)(${r.c}))`, t: "num" } : { c: `lc_unm(${toLv(r)})`, t: "lv" };
        case "~": return r.t === "num" ? { c: `(~(${r.c}))`, t: "num" } : { c: `lc_arith(LC_OPBNOT, ${toLv(r)}, LV_NIL)`, t: "lv" };
        case "#": return { c: `lc_lenv(${toLv(r)})`, t: "lv" };
        case "@": return { c: `((int32_t)p8_rd8((uint16_t)(int16_t)(${toNum(r)} >> 16)) << 16)`, t: "num" };
        case "%": return { c: `((int32_t)lc_p8_peek2((uint16_t)(int16_t)(${toNum(r)} >> 16)) << 16)`, t: "num" };
        case "$": return { c: `lc_p8_peek4((uint16_t)(int16_t)(${toNum(r)} >> 16))`, t: "num" };
        default: throw new Error("unop " + e.op);
      }
    }

    const ARITH = {
      "+": ["lc_add", (a, b) => `(int32_t)((uint32_t)(${a}) + (uint32_t)(${b}))`],
      "-": ["lc_sub", (a, b) => `(int32_t)((uint32_t)(${a}) - (uint32_t)(${b}))`],
      "*": ["lc_mul", (a, b) => `lc_fmul(${a}, ${b})`],
      "/": ["lc_div", (a, b) => `lc_fdiv(${a}, ${b})`],
      "%": ["lc_mod", (a, b) => `lc_fmod(${a}, ${b})`],
      "\\": ["lc_idiv", (a, b) => `lc_fidiv(${a}, ${b})`],
      "^": ["LC_OPPOW", (a, b) => `lc_fpow(${a}, ${b})`],
      "&": ["LC_OPBAND", (a, b) => `((${a}) & (${b}))`],
      "|": ["LC_OPBOR", (a, b) => `((${a}) | (${b}))`],
      "^^": ["LC_OPBXOR", (a, b) => `((${a}) ^ (${b}))`],
      "<<": ["LC_OPSHL", (a, b) => `lc_fshl(${a}, ${b})`],
      ">>": ["LC_OPSHR", (a, b) => `lc_fshr(${a}, ${b})`],
      ">>>": ["LC_OPLSHR", (a, b) => `lc_flshr(${a}, ${b})`],
      "<<>": ["LC_OPROTL", (a, b) => `lc_frotl(${a}, ${b})`],
      ">><": ["LC_OPROTR", (a, b) => `lc_frotr(${a}, ${b})`],
    };

    // evaluate two operands honouring Lua's left-to-right order
    function operands(e) {
      const a = ex(e.left), b = ex(e.right);
      if (!hasCall(e.right) || (e.left.kind === "Number" || e.left.kind === "String" || e.left.kind === "Nil")) return { a, b, pre: "" };
      const t = T();
      const decl = a.t === "num" ? `int32_t ${t} = ${a.c};` : a.t === "bool" ? `int ${t} = ${a.c};` : `lv ${t} = ${toLv(a)};`;
      return { a: { c: t, t: a.t === "num" ? "num" : a.t === "bool" ? "bool" : "lv" }, b, pre: decl };
    }
    const wrap = (pre, c) => (pre ? `({ ${pre} ${c}; })` : c);

    function binop(e) {
      const op = e.op;
      if (e.left.kind === "Number" && e.right.kind === "Number") {
        const v = foldBin(op, e.left.bits, e.right.bits);
        if (v !== null) return { c: numLit(v), t: "num" };
      }
      if (op === "and" || op === "or") {
        const a = ex(e.left), b = ex(e.right);
        const t = T();
        if (a.t === "num") return op === "and" ? { c: `((void)(${a.c}), ${b.c})`, t: b.t } : a;
        const decl = a.t === "bool" ? `int ${t} = ${a.c};` : `lv ${t} = ${toLv(a)};`;
        const cond = a.t === "bool" ? t : `LV_TRUTHY(${t})`;
        if (a.t === "bool" && b.t === "bool") {
          return op === "and" ? { c: `(${a.c} && ${b.c})`, t: "bool" } : { c: `(${a.c} || ${b.c})`, t: "bool" };
        }
        const av = a.t === "bool" ? `LV_BOOL(${t})` : t;
        return op === "and"
          ? { c: `({ ${decl} ${cond} ? ${toLv(b)} : ${av}; })`, t: "lv" }
          : { c: `({ ${decl} ${cond} ? ${av} : ${toLv(b)}; })`, t: "lv" };
      }
      const { a, b, pre } = operands(e);
      if (ARITH[op]) {
        const [slow, fastf] = ARITH[op];
        if (a.t === "num" && b.t === "num") return { c: wrap(pre, fastf(a.c, b.c)), t: "num" };
        if (slow.startsWith("LC_OP")) return { c: wrap(pre, `lc_arith(${slow}, ${toLv(a)}, ${toLv(b)})`), t: "lv" };
        return { c: wrap(pre, `${slow}(${toLv(a)}, ${toLv(b)})`), t: "lv" };
      }
      if (op === "..") return { c: wrap(pre, `lc_concat(${toLv(a)}, ${toLv(b)})`), t: "lv" };
      const bothNum = a.t === "num" && b.t === "num";
      switch (op) {
        case "==": return { c: wrap(pre, bothNum ? `((${a.c}) == (${b.c}))` : `lc_eqv(${toLv(a)}, ${toLv(b)})`), t: "bool" };
        case "~=": return { c: wrap(pre, bothNum ? `((${a.c}) != (${b.c}))` : `(!lc_eqv(${toLv(a)}, ${toLv(b)}))`), t: "bool" };
        case "<": return { c: wrap(pre, bothNum ? `((${a.c}) < (${b.c}))` : `lc_ltv(${toLv(a)}, ${toLv(b)})`), t: "bool" };
        case "<=": return { c: wrap(pre, bothNum ? `((${a.c}) <= (${b.c}))` : `lc_lev(${toLv(a)}, ${toLv(b)})`), t: "bool" };
        case ">": {
          if (bothNum) return { c: wrap(pre, `((${a.c}) > (${b.c}))`), t: "bool" };
          const ta = T(), tb2 = T();
          return { c: `({ ${pre} lv ${ta} = ${toLv(a)}; lv ${tb2} = ${toLv(b)}; lc_ltv(${tb2}, ${ta}); })`, t: "bool" };
        }
        case ">=": {
          if (bothNum) return { c: wrap(pre, `((${a.c}) >= (${b.c}))`), t: "bool" };
          const ta = T(), tb2 = T();
          return { c: `({ ${pre} lv ${ta} = ${toLv(a)}; lv ${tb2} = ${toLv(b)}; lc_lev(${tb2}, ${ta}); })`, t: "bool" };
        }
        default: throw new Error("binop " + op);
      }
    }

    // ---- statements -----------------------------------------------------------------------

    function assignTo(t, r) {
      // t already resolved; r is the value result
      if (t.kind === "Name") {
        const ref = t.ref;
        if (ref.kind === "local") return assignVar(ref.v, r);
        if (ref.kind === "env") return out(`lc_setfield(${toLv(vread(ref.v))}, ${lit(t.name)}, ${toLv(r)});`);
        if (ref.kind === "genv") return out(`GENV = ${toLv(r)};`);
        if (tableMode) return out(`lc_setfield(GENV, ${lit(t.name)}, ${toLv(r)});`);
        return out(`G[${gslot.get(t.name)}] = ${toLv(r)};`);
      }
      throw new Error("assignTo index");
    }

    function stmtAssign(s) {
      if (s.targets.length === 1 && s.exprs.length === 1) {
        const t = s.targets[0];
        if (t.kind === "Name") { assignTo(t, ex(s.exprs[0])); return; }
        const o = T("o");
        out(`{ lv ${o} = ${toLv(ex(t.obj))};`);
        if (t.key.kind === "String") {
          out(`  lc_setfield(${o}, ${lit(t.key.value)}, ${toLv(ex(s.exprs[0]))}); }`);
        } else {
          const k = T("k");
          out(`  lv ${k} = ${toLv(ex(t.key))};`);
          out(`  lc_seti(${o}, ${k}, ${toLv(ex(s.exprs[0]))}); }`);
        }
        return;
      }
      // general: evaluate target prefixes, then all values, then assign
      out("{");
      indent++;
      const tinfo = s.targets.map((t) => {
        if (t.kind === "Name") return { t };
        const o = T("o"), k = T("k");
        out(`lv ${o} = ${toLv(ex(t.obj))};`);
        out(`lv ${k} = ${toLv(t.key.kind === "String" ? { c: `LV_STR(${lit(t.key.value)})`, t: "lv" } : ex(t.key))};`);
        return { t, o, k };
      });
      const vals = evalList(s.exprs, s.targets.length);
      for (let i = tinfo.length - 1; i >= 0; i--) {
        const ti = tinfo[i];
        if (ti.o) out(`lc_seti(${ti.o}, ${ti.k}, ${vals[i]});`);
        else assignTo(ti.t, { c: vals[i], t: "lv" });
      }
      indent--;
      out("}");
    }

    // evaluate an expression list adjusted to `want` values into temporaries;
    // returns the temp names (lv)
    function evalList(exprs, want) {
      const names = [];
      for (let i = 0; i < exprs.length && names.length < want; i++) {
        const e = exprs[i];
        const last = i === exprs.length - 1;
        if (last && isMulti(e) && names.length < want) {
          const need = want - names.length;
          if (e.kind === "Vararg") {
            for (let j = 0; j < need; j++) { const t = T(); out(`lv ${t} = NVA > ${j} ? VA[${j}] : LV_NIL;`); names.push(t); }
          } else {
            const b = T("b"), m = T("m");
            out(`lv *${b} = lc_top; int ${m} = ${callMulti(e)};`);
            for (let j = 0; j < need; j++) { const t = T(); out(`lv ${t} = ${m} > ${j} ? ${b}[${j}] : LV_NIL;`); names.push(t); }
            out(`lc_top = ${b};`);
          }
          return names;
        }
        const t = T();
        out(`lv ${t} = ${toLv(ex(e))};`);
        names.push(t);
      }
      // evaluate (and discard) extra expressions
      for (let i = names.length; i < exprs.length; i++) {
        if (i >= want) out(`(void)(${toLv(ex(exprs[i]))});`);
      }
      while (names.length < want) { const t = T(); out(`lv ${t} = LV_NIL;`); names.push(t); }
      return names;
    }

    function stmtLocal(s) {
      const vars = s.vars;
      if (vars.length === 1 && s.exprs.length <= 1) {
        if (s.exprs.length === 0) { declInit(vars[0], { c: "LV_NIL", t: "lv" }); return; }
        const e = s.exprs[0];
        declInit(vars[0], ex(e));
        return;
      }
      const vals = evalList(s.exprs, vars.length);
      vars.forEach((v, i) => declInit(v, { c: vals[i], t: "lv" }));
    }

    function stmtCompound(s) {
      const op = s.op;
      if (s.target.kind === "Name") {
        assignTo(s.target, binopRaw(op, nameRead(s.target), s.value));
        return;
      }
      const o = T("o"), k = T("k");
      out(`{ lv ${o} = ${toLv(ex(s.target.obj))};`);
      const keyR = s.target.key.kind === "String" ? { c: `LV_STR(${lit(s.target.key.value)})`, t: "lv" } : ex(s.target.key);
      out(`  lv ${k} = ${toLv(keyR)};`);
      const r = binopRaw(op, { c: `lc_idx(${o}, ${k})`, t: "lv" }, s.value);
      out(`  lc_seti(${o}, ${k}, ${toLv(r)}); }`);
    }
    // binop with an already-evaluated left operand
    function binopRaw(op, left, rightE) {
      const lt = T();
      const pre = left.t === "num" ? `int32_t ${lt} = ${left.c};` : `lv ${lt} = ${toLv(left)};`;
      const a = { c: lt, t: left.t === "num" ? "num" : "lv" };
      const b = ex(rightE);
      let body;
      if (ARITH[op]) {
        const [slow, fastf] = ARITH[op];
        if (a.t === "num" && b.t === "num") return { c: `({ ${pre} ${fastf(a.c, b.c)}; })`, t: "num" };
        body = slow.startsWith("LC_OP") ? `lc_arith(${slow}, ${toLv(a)}, ${toLv(b)})` : `${slow}(${toLv(a)}, ${toLv(b)})`;
      } else if (op === "..") body = `lc_concat(${toLv(a)}, ${toLv(b)})`;
      else throw new Error("compound op " + op);
      return { c: `({ ${pre} ${body}; })`, t: "lv" };
    }

    function stmtIf(s) {
      s.clauses.forEach((cl, i) => {
        out(`${i === 0 ? "if" : "else if"} (${toCond(ex(cl.cond))}) {`);
        indent++;
        blockBody(cl.body);
        indent--;
        out("}");
      });
      if (s.orelse) {
        out("else {");
        indent++;
        blockBody(s.orelse);
        indent--;
        out("}");
      }
    }

    function stmtNumFor(s) {
      const i = T("fi"), l = T("fl"), st = T("fs"), nx = T("fn");
      const startR = ex(s.start), limR = ex(s.limit);
      const conv = (r, what) => (r.t === "num" ? r.c : `lc_fornum(${toLv(r)}, "'for' ${what} must be a number")`);
      out("{");
      indent++;
      out(`int32_t ${i} = ${conv(startR, "initial value")};`);
      out(`int32_t ${l} = ${conv(limR, "limit")};`);
      const constStep = !s.step ? 0x10000 : (s.step.kind === "Number" ? s.step.bits : (s.step.kind === "Unop" && s.step.op === "-" && s.step.expr.kind === "Number" ? (-s.step.expr.bits | 0) : null));
      if (constStep !== null) out(`const int32_t ${st} = ${numLit(constStep)};`);
      else out(`int32_t ${st} = ${conv(ex(s.step), "step")};`);
      out(`${i} = (int32_t)((uint32_t)${i} - (uint32_t)${st});`);
      out("for (;;) {");
      indent++;
      out(`int32_t ${nx} = (int32_t)((uint32_t)${i} + (uint32_t)${st});`);
      if (constStep !== null && constStep > 0) out(`if (!(${i} <= ${nx} && ${nx} <= ${l})) break;`);
      else if (constStep !== null) out(`if (!(${nx} <= ${i} && ${l} <= ${nx})) break;`);
      else out(`if (${st} > 0 ? !(${i} <= ${nx} && ${nx} <= ${l}) : !(${nx} <= ${i} && ${l} <= ${nx})) break;`);
      out(`${i} = ${nx};`);
      declInit(s.var, { c: i, t: "num" });
      loopStack.push(true);
      blockBody(s.body);
      loopStack.pop();
      indent--;
      out("}");
      indent--;
      out("}");
    }

    function isCallOf(e, bname, nargs) {
      return e && e.kind === "Call" && isPureBuiltin(e.fn, res) && e.fn.name === bname &&
        e.args.length === nargs && !isMulti(e.args[nargs - 1] ?? { kind: "x" });
    }

    function stmtGenFor(s) {
      out("{");
      indent++;
      const vars = s.vars;
      if (s.exprs.length === 1 && isCallOf(s.exprs[0], "pairs", 1)) {
        const tv = T("pt"), k = T("pk"), v = T("pv"), fast = T("pf"), gf = T("gf"), gs = T("gs"), b = T("b"), n = T("n");
        out(`lv ${tv} = ${toLv(ex(s.exprs[0].args[0]))};`);
        out(`int ${fast} = lc_pairs_fast(${tv}); lv ${gf} = LV_NIL, ${gs} = LV_NIL, ${k} = LV_NIL, ${v} = LV_NIL;`);
        out(`if (!${fast}) { lv *${b} = lc_top; ${b}[0] = ${tv}; lc_top = ${b} + 1; int ${n} = lcb_pairs(${b}, 1, 0); ${gf} = ${b}[0]; ${gs} = ${n} > 1 ? ${b}[1] : LV_NIL; ${k} = ${n} > 2 ? ${b}[2] : LV_NIL; lc_top = ${b}; }`);
        out("for (;;) {");
        indent++;
        out(`if (${fast}) { if (!lc_tab_next((lc_tab *)LV_PTR(${tv}), &${k}, &${v})) break; }`);
        out(`else { lv *${b} = lc_top; ${b}[0] = ${gs}; ${b}[1] = ${k}; lc_top = ${b} + 2; int ${n} = lc_call(${gf}, ${b}, 2); ${k} = ${n} > 0 ? ${b}[0] : LV_NIL; ${v} = ${n} > 1 ? ${b}[1] : LV_NIL; lc_top = ${b}; if (${k} == LV_NIL) break; }`);
        declInit(vars[0], { c: k, t: "lv" });
        if (vars[1]) declInit(vars[1], { c: v, t: "lv" });
        for (let j = 2; j < vars.length; j++) declInit(vars[j], { c: "LV_NIL", t: "lv" });
      } else if (s.exprs.length === 1 && isCallOf(s.exprs[0], "all", 1)) {
        const c = T("ac"), i = T("ai"), p = T("ap"), n = T("an");
        out(`lv ${c} = ${toLv(ex(s.exprs[0].args[0]))};`);
        out(`int32_t ${i} = 1; lv ${p} = LV_NIL;`);
        out(`if (${c} != LV_NIL && LV_N(lc_lenv(${c})) != 0) for (;;) {`);
        indent++;
        out(`if (lc_eqv(lc_idx(${c}, LV_INT(${i})), ${p})) ${i}++;`);
        out(`int32_t ${n} = LV_N(lc_lenv(${c})) >> 16;`);
        out(`while (${i} <= ${n} && lc_idx(${c}, LV_INT(${i})) == LV_NIL) ${i}++;`);
        out(`${p} = lc_idx(${c}, LV_INT(${i}));`);
        out(`if (${p} == LV_NIL) break;`);
        declInit(vars[0], { c: p, t: "lv" });
        for (let j = 1; j < vars.length; j++) declInit(vars[j], { c: "LV_NIL", t: "lv" });
      } else if (s.exprs.length === 1 && isCallOf(s.exprs[0], "ipairs", 1)) {
        const tv = T("it"), i = T("ii"), v = T("iv"), fast = T("pf"), gf = T("gf"), gs = T("gs"), gk = T("gk"), b = T("b"), n = T("n");
        out(`lv ${tv} = ${toLv(ex(s.exprs[0].args[0]))};`);
        out(`int ${fast} = lc_ipairs_fast(${tv}); lv ${gf} = LV_NIL, ${gs} = LV_NIL, ${gk} = LV_NIL;`);
        out(`if (!${fast}) { lv *${b} = lc_top; ${b}[0] = ${tv}; lc_top = ${b} + 1; int ${n} = lcb_ipairs(${b}, 1, 0); ${gf} = ${b}[0]; ${gs} = ${n} > 1 ? ${b}[1] : LV_NIL; ${gk} = ${n} > 2 ? ${b}[2] : LV_NIL; lc_top = ${b}; }`);
        out(`int32_t ${i} = 0;`);
        out("for (;;) {");
        indent++;
        out(`lv ${v};`);
        out(`if (${fast}) { ${i}++; ${v} = lc_tab_getint((lc_tab *)LV_PTR(${tv}), ${i}); if (${v} == LV_NIL) break; ${gk} = LV_INT(${i}); }`);
        out(`else { lv *${b} = lc_top; ${b}[0] = ${gs}; ${b}[1] = ${gk}; lc_top = ${b} + 2; int ${n} = lc_call(${gf}, ${b}, 2); ${gk} = ${n} > 0 ? ${b}[0] : LV_NIL; ${v} = ${n} > 1 ? ${b}[1] : LV_NIL; lc_top = ${b}; if (${gk} == LV_NIL) break; }`);
        declInit(vars[0], { c: gk, t: "lv" });
        if (vars[1]) declInit(vars[1], { c: v, t: "lv" });
        for (let j = 2; j < vars.length; j++) declInit(vars[j], { c: "LV_NIL", t: "lv" });
      } else {
        const [gf, gs, gv] = evalList(s.exprs, 3);
        out("for (;;) {");
        indent++;
        const b = T("b"), n = T("n");
        out(`lv *${b} = lc_top; ${b}[0] = ${gs}; ${b}[1] = ${gv}; lc_top = ${b} + 2;`);
        out(`int ${n} = lc_call(${gf}, ${b}, 2);`);
        const ks = vars.map((_, j) => { const t = T(); out(`lv ${t} = ${n} > ${j} ? ${b}[${j}] : LV_NIL;`); return t; });
        out(`lc_top = ${b};`);
        out(`if (${ks[0]} == LV_NIL) break;`);
        out(`${gv} = ${ks[0]};`);
        vars.forEach((v, j) => declInit(v, { c: ks[j], t: "lv" }));
      }
      loopStack.push(true);
      blockBody(s.body);
      loopStack.pop();
      indent--;
      out("}");
      indent--;
      out("}");
    }

    function stmtReturn(s) {
      const ex_ = s.exprs;
      if (ex_.length === 0) { out("return 0;"); return; }
      if (ex_.length === 1 && !isMulti(ex_[0])) {
        const r = ex(ex_[0]);
        out(`{ lv r_ = ${toLv(r)}; B[0] = r_; return 1; }`);
        return;
      }
      out("{");
      indent++;
      const fixed = [];
      for (let i = 0; i < ex_.length; i++) {
        const e = ex_[i];
        if (i === ex_.length - 1 && isMulti(e)) {
          if (e.kind === "Vararg") {
            fixed.forEach((t, j) => out(`B[${j}] = ${t};`));
            out(`lc_movev(B + ${fixed.length}, VA, NVA); return ${fixed.length} + NVA;`);
          } else {
            const b = T("b"), m = T("m");
            out(`lv *${b} = lc_top; int ${m} = ${callMulti(e)};`);
            // results sit at b; fixed values go first
            out(`lc_movev(B + ${fixed.length}, ${b}, ${m});`);
            fixed.forEach((t, j) => out(`B[${j}] = ${t};`));
            out(`return ${fixed.length} + ${m};`);
          }
          indent--;
          out("}");
          return;
        }
        const t = T();
        out(`lv ${t} = ${toLv(ex(e))};`);
        fixed.push(t);
      }
      fixed.forEach((t, j) => out(`B[${j}] = ${t};`));
      out(`return ${fixed.length};`);
      indent--;
      out("}");
    }

    function blockBody(b) {
      // labels in this block are visible to gotos anywhere in it (and nested)
      const labels = new Map();
      for (const s of b.stmts) if (s.kind === "Label") labels.set(s.name, `L_${f.id}_${T("")}`);
      labelStack.push(labels);
      for (const s of b.stmts) stmt(s);
      labelStack.pop();
    }

    function stmt(s) {
      if (opts.debugLines && s.line) out(`lc_curline = ${s.line};`);
      switch (s.kind) {
        case "Local": stmtLocal(s); break;
        case "LocalFunction": {
          const v = s.var;
          if (v.isStatic) { out(`${vread(v).c} = ${toLv(closure(s.func.fn))};`); break; }
          // declared before the closure exists so the function can see itself
          if (v.captured) {
            out(`lc_cell *c${v.id} = lc_cell_new(LV_NIL);`);
            out(`c${v.id}->v = ${toLv(closure(s.func.fn))};`);
          } else out(`lv l${v.id} = ${toLv(closure(s.func.fn))};`);
          break;
        }
        case "Assign": stmtAssign(s); break;
        case "CompoundAssign": stmtCompound(s); break;
        case "CallStat": {
          const fast = fastBuiltin(s.call);
          if (fast) out(`(void)(${fast.c});`);
          else out(`${call(s.call, "stmt").c};`);
          break;
        }
        case "Do": out("{"); indent++; blockBody(s.body); indent--; out("}"); break;
        case "While":
          out("for (;;) {");
          indent++;
          out(`if (!(${toCond(ex(s.cond))})) break;`);
          loopStack.push(true);
          blockBody(s.body);
          loopStack.pop();
          indent--;
          out("}");
          break;
        case "Repeat":
          out("for (;;) {");
          indent++;
          loopStack.push(true);
          {
            // the condition sees the body's locals: same C block
            const labels = new Map();
            for (const st of s.body.stmts) if (st.kind === "Label") labels.set(st.name, `L_${f.id}_${T("")}`);
            labelStack.push(labels);
            for (const st of s.body.stmts) stmt(st);
            out(`if (${toCond(ex(s.cond))}) break;`);
            labelStack.pop();
          }
          loopStack.pop();
          indent--;
          out("}");
          break;
        case "If": stmtIf(s); break;
        case "NumericFor": stmtNumFor(s); break;
        case "GenericFor": stmtGenFor(s); break;
        case "Return": stmtReturn(s); break;
        case "Break":
          if (!loopStack.length) { diags.push({ file, line: s.line, col: s.col, severity: "error", message: "<break> at line " + s.line + " not inside a loop" }); break; }
          out("break;");
          break;
        case "Goto": {
          let target = null;
          for (let k = labelStack.length - 1; k >= 0; k--) if (labelStack[k].has(s.name)) { target = labelStack[k].get(s.name); break; }
          if (!target) { diags.push({ file, line: s.line, col: s.col, severity: "error", message: `no visible label '${s.name}' for goto` }); break; }
          out(`goto ${target};`);
          break;
        }
        case "Label": out(`${labelStack[labelStack.length - 1].get(s.name)}: ;`); break;
        default: throw new Error("unknown statement " + s.kind);
      }
    }

    // ---- prologue
    const np = node.params.length;
    out("LC_ENTER(32);");
    (node.paramVars ?? []).forEach((v, i) => {
      const src = `(N > ${i} ? B[${i}] : LV_NIL)`;
      if (v.captured) out(`lc_cell *c${v.id} = 0; lv p${v.id} = ${src};`);
      else if (v.num) out(`int32_t n${v.id} = lc_n(${src});`);
      else out(`lv l${v.id} = ${src};`);
    });
    if (node.vararg) out(`lv *VA = B + ${np}; int NVA = N > ${np} ? N - ${np} : 0; lc_top = B + (N > ${np} ? N : ${np});`);
    else out("lc_top = B;");
    (node.paramVars ?? []).forEach((v) => { if (v.captured) out(`c${v.id} = lc_cell_new(p${v.id});`); });
    if (!node.vararg) out("(void)N;");
    out("(void)C;");
    blockBody(node.body);
    out("return 0;");
    const sig = `static int ${fname(f)}(lv *B, int N, lc_clo *C)`;
    protos.push(`${sig};`);
    if (f.upvals.length === 0) protos.push(`LC_FNLIT(${fclo(f)}, ${fname(f)}, ${cstr(node.name ?? "?")});`);
    fnOut.push(`/* ${node.name ?? "function"} (line ${node.line}) */\n${sig} {\n${lines.join("\n")}\n}\n`);
  }

  for (const f of res.fns) emitFunction(f);
  if (diags.some((d) => d.severity === "error")) return { c: null, diagnostics: diags };

  // ---- assemble ------------------------------------------------------------------------
  const outp = [];
  outp.push(`/* generated by luacretro (dynamic tier) from ${file} - do not edit */`);
  outp.push('#include "lc.h"', '#include "lc_lib.h"', '#include "lc_p8.h"', '#include "lc_cart.h"', "");
  outp.push("#if defined(__GNUC__)\n#pragma GCC diagnostic ignored \"-Wunused-variable\"\n#pragma GCC diagnostic ignored \"-Wunused-value\"\n#pragma GCC diagnostic ignored \"-Wunused-but-set-variable\"\n#pragma GCC diagnostic ignored \"-Wunused-label\"\n#pragma GCC diagnostic ignored \"-Wunused-const-variable\"\n#endif");
  // literals needed by setup
  for (const n of ["_init", "_update", "_update60", "_draw"]) lit(n);
  for (const n of Object.keys(BUILTINS)) if (tableMode) lit(n);
  for (const n of Object.keys(GLYPH_GLOBALS)) if (tableMode) lit(n);
  // setup (built first: it may add literals)
  const setup = [];
  setup.push("void lc_cart_setup(void) {");
  setup.push("  /*INTERN*/");
  setup.push(`  for (int i = 0; i < ${Math.max(1, gslot.size)}; i++) G[i] = LV_NIL;`);
  setup.push(`  for (int i = 0; i < ${Math.max(1, nstatic)}; i++) S[i] = LV_NIL;`);
  setup.push(`  lc_register_roots(G, ${Math.max(1, gslot.size)});`);
  setup.push(`  lc_register_roots(S, ${Math.max(1, nstatic)});`);
  setup.push("  lc_register_roots(&GENV, 1);");
  for (const v of res.apiVars.values()) if (v.staticIdx >= 0) setup.push(`  S[${v.staticIdx}] = LV_FUN(&lcb_${BUILTINS[v.name]}_clo);`);
  if (tableMode) {
    setup.push("  { lc_tab *e = lc_tab_new(0, 0); GENV = LV_TAB(e);");
    for (const [n, sym] of Object.entries(BUILTINS)) setup.push(`    lc_tab_set(e, LV_STR(${lit(n)}), LV_FUN(&lcb_${sym}_clo));`);
    for (const [g, bits] of Object.entries(GLYPH_GLOBALS)) setup.push(`    lc_tab_set(e, LV_STR(${lit(g)}), LV_NUM(${numLit(bits)}));`);
    setup.push("  }");
  } else {
    for (const [n, slot] of gslot) {
      if (Object.prototype.hasOwnProperty.call(BUILTINS, n)) setup.push(`  G[${slot}] = LV_FUN(&lcb_${BUILTINS[n]}_clo);`);
      else if (Object.prototype.hasOwnProperty.call(GLYPH_GLOBALS, n)) setup.push(`  G[${slot}] = LV_NUM(${numLit(GLYPH_GLOBALS[n])});`);
    }
  }
  setup.push("}");
  const cbLines = [];
  cbLines.push("lv lc_cart_cb(int which) {");
  cbLines.push("  switch (which) {");
  ["_init", "_update", "_update60", "_draw"].forEach((n, i) => {
    cbLines.push(tableMode ? `    case ${i}: return lc_fld(GENV, ${lit(n)});` : `    case ${i}: return G[${gslot.get(n)}];`);
  });
  cbLines.push("  }", "  return LV_NIL;", "}", "");
  const litLines = [];
  const intern = [];
  for (const [s, cn] of strings) {
    litLines.push(`LC_STRLIT(${cn}, ${hex32(strHash(s))}, ${s.length}, ${cstr(s)});`);
    intern.push(`  lc_str_intern_static((const lc_str *)&${cn});`);
  }
  setup[1] = intern.join("\n");
  outp.push(...litLines, "");
  outp.push(`static lv G[${Math.max(1, gslot.size)}];`);
  outp.push(`static lv S[${Math.max(1, nstatic)}];`);
  outp.push("static lv GENV;", "");
  outp.push(...protos, "");
  outp.push(...fnOut);
  outp.push(...setup, "");
  outp.push("void lc_cart_main(void) {", "  lv *b = lc_top;", `  ${fname(res.fns[0])}(b, 0, 0);`, "  lc_top = b;", "}", "");
  outp.push(...cbLines);
  const rom = opts.rom ?? new Uint8Array(0x4300);
  const romBytes = [];
  for (let i = 0; i < 0x4300; i++) romBytes.push(rom[i] ?? 0);
  const romLines = [];
  for (let i = 0; i < romBytes.length; i += 32) romLines.push("  " + romBytes.slice(i, i + 32).join(","));
  outp.push("const uint8_t lc_cart_rom[0x4300] = {", romLines.join(",\n"), "};", "");
  return { c: outp.join("\n"), diagnostics: diags };
}
