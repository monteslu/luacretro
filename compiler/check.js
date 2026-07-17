// gbalua semantic checker - scopes, arity, and the numeric-kind system.
//
// Numbers are PICO-8 16.16 fixed point semantically. The compiler tracks two
// KINDS underneath: "int" (provably integral, 16-bit C int - fast on the
// 6502) and "fixed" (32-bit 16.16, C long). Kinds are inferred to a fixpoint:
// a slot (global, local, param, return) starts int and widens to fixed if any
// fractional value flows into it. int arithmetic wraps at the same boundaries
// as PICO-8's 16 integer bits, so the optimization is semantically invisible.
//
// Conditions must be boolean (deliberate wall: Lua's 0 is truthy, C's is not).

import { BUILTINS, CALLBACKS } from "./builtins.js";

const join = (a, b) => (a === "fixed" || b === "fixed") ? "fixed" : "int";

export function check(chunk, file) {
  const diagnostics = [];
  const globals = new Map();   // name -> {kind, fixedInit, node}
  const usesAudio = { flag: false };
  const usesMusic = { flag: false };   // sfx()/music() -> link gt_music.o
  const functions = new Map(); // name -> {params:[names], paramKinds:[], ret, retKind, node}
  let reporting = true;        // pass 1 runs once: report; fixpoint passes: quiet
  let changed = false;         // fixpoint tracker
  let rndListSeq = 0;          // unique names for rnd({...}) const lists

  const err = (node, msg) => {
    if (reporting) diagnostics.push({ file, line: node?.line ?? 0, col: node?.col ?? 0, severity: "error", message: msg });
  };
  const warn = (node, msg) => {
    if (reporting) diagnostics.push({ file, line: node?.line ?? 0, col: node?.col ?? 0, severity: "warning", message: msg });
  };

  const isPow2 = (n) => Number.isInteger(n) && n > 0 && (n & (n - 1)) === 0;

  // Constant folding over VALUES (JS numbers, may be fractional).
  function constEval(e) {
    if (!e) return null;
    switch (e.kind) {
      case "number": return e.value;
      case "bool": return null;
      case "neg": {
        const v = constEval(e.expr);
        return v === null ? null : -v;
      }
      case "binop": {
        const l = constEval(e.left), r = constEval(e.right);
        if (l === null || r === null) return null;
        switch (e.op) {
          case "+": return l + r;
          case "-": return l - r;
          case "*": return l * r;
          case "/": return r === 0 ? null : l / r;
          case "\\": return r === 0 ? null : Math.floor(l / r);
          case "%": return r === 0 ? null : l - Math.floor(l / r) * r;
          default: return null;
        }
      }
      default: return null;
    }
  }

  // ---- pass 1: collect top-level declarations -------------------------------
  for (const s of chunk.stmts) {
    if (s.kind === "function") {
      if (functions.has(s.name) || globals.has(s.name)) { err(s, `'${s.name}' is already defined`); continue; }
      if (BUILTINS[s.name]) { /* allowed to shadow? no */ }
      functions.set(s.name, {
        params: s.params,
        paramKinds: s.params.map(() => "int"),
        retKind: "int",
        hasReturnValue: false,
        node: s,
      });
    } else if (s.kind === "local") {
      s.names.forEach((name, idx) => {
        if (globals.has(name) || functions.has(name)) { err(s, `'${name}' is already defined`); return; }
        const init = s.inits[idx] ?? null;
        // struct pool: local bullets = pool(N)
        if (init && init.kind === "call" && init.callee.kind === "name" && init.callee.name === "pool") {
          const size = constEval(init.args[0]);
          if (size === null || !Number.isInteger(size) || size < 1 || size > 64) {
            err(s, "pool(n) needs a constant capacity between 1 and 64");
          }
          // pool(n, "f1,f2,...") - the listed fields are DECLARED byte-wide
          // (values 0-255, stored in one byte, ~2-3x faster per access on
          // the 65C02). Explicit like array8: the compiler trusts the list
          // and errors only if a listed field turns out fixed-typed.
          const byteFields = new Set();
          if (init.args[1]) {
            if (init.args[1].kind !== "string") {
              err(init.args[1], 'pool(n, "fields") takes a comma-separated string of byte-wide field names');
            } else {
              for (const f of init.args[1].value.split(",")) {
                const t = f.trim();
                if (t) byteFields.add(t);
              }
            }
          }
          globals.set(name, {
            kind: "pool",
            size: size ?? 1,
            fields: new Map(),   // fieldName -> {kind}
            byteFields,
            node: s,
          });
          return;
        }
        // fixed-capacity array: local pool = array(N [, initValue]).
        // array8(N [, init]) is the byte variant: elements are 0-255 stored in
        // ONE byte each - half the RAM and roughly half the cycles per access
        // on the 65C02 (single-register load, no high-byte traffic). Values
        // read back as ordinary ints; stores must be integers (flr() first).
        if (init && init.kind === "call" && init.callee.kind === "name" &&
            init.callee.name === "hexdata") {
          const a = init.args[0];
          if (init.args.length !== 1 || !a || a.kind !== "string" ||
              a.value.length === 0 || a.value.length % 2 !== 0 ||
              /[^0-9a-fA-F]/.test(a.value)) {
            err(s, "hexdata(\"...\") needs one even-length hex string literal");
            return;
          }
          const data = [];
          for (let k = 0; k < a.value.length; k += 2) {
            data.push(parseInt(a.value.slice(k, k + 2), 16));
          }
          globals.set(name, {
            kind: "array",
            elemKind: "int",
            elemBytes: true,
            size: data.length,
            initVal: 0,
            hexdata: data,
            node: s,
          });
          return;
        }
        if (init && init.kind === "call" && init.callee.kind === "name" &&
            (init.callee.name === "array" || init.callee.name === "array8")) {
          const bytes = init.callee.name === "array8";
          const size = constEval(init.args[0]);
          const iv = init.args[1] ? constEval(init.args[1]) : 0;
          if (size === null || !Number.isInteger(size) || size < 1 || size > 4096) {
            err(s, `${init.callee.name}(n) needs a constant capacity between 1 and 4096`);
          }
          if (init.args[1] && iv === null) {
            err(s, `${init.callee.name}(n, v) initial value must be a constant`);
          }
          const ivv = iv ?? 0;
          if (bytes && (!Number.isInteger(ivv) || ivv < 0 || ivv > 255)) {
            err(s, "array8(n, v) initial value must be an integer 0-255");
          }
          globals.set(name, {
            kind: "array",
            elemKind: bytes ? "int" : (Number.isInteger(ivv) ? "int" : "fixed"),
            elemBytes: bytes,
            size: size ?? 1,
            initVal: ivv,
            node: s,
          });
          return;
        }
        // constant array table: local levels = {1, 2, 3}. A fixed C array of
        // its (constant) values - the read-only data-table idiom (palettes,
        // level lists, lookup rows). Each element must fold to a constant here;
        // a runtime value or a nested table is not a flat array we can emit.
        if (init && init.kind === "arraytable") {
          if (init.elements.length === 0) {
            err(s, "empty array table {} has no size - use array(n) for a runtime-filled array");
            return;
          }
          const vals = [];
          let anyFixed = false, bad = null;
          for (const el of init.elements) {
            const v = constEval(el);
            if (v === null) { bad = el; break; }
            if (!Number.isInteger(v)) anyFixed = true;
            vals.push(v);
          }
          if (bad) {
            err(bad, "array table elements must be constants (a number, or arithmetic on numbers); " +
                     "for a runtime-filled array use array(n) and assign in _init()");
            return;
          }
          const allByte = !anyFixed && vals.every((v) => v >= 0 && v <= 255);
          globals.set(name, {
            kind: "array",
            elemKind: anyFixed ? "fixed" : "int",
            elemBytes: allByte,
            size: vals.length,
            initVal: 0,
            initList: vals,
            node: s,
          });
          return;
        }
        // top-level 'local x = nil': an int global starting at the sentinel.
        if (init && init.kind === "nil") {
          globals.set(name, { kind: "int", value: 0, nilInit: true, node: s });
          return;
        }
        const cv = init === null ? 0 : constEval(init);
        if (init !== null && cv === null) {
          err(s, `top-level 'local ${name}' must be initialized with a constant expression ` +
                 `(runtime init belongs in function _init())`);
        }
        const value = cv ?? 0;
        const isInt = Number.isInteger(value) && value >= -32768 && value <= 32767;
        globals.set(name, {
          kind: isInt ? "int" : "fixed",
          value,
          node: s,
        });
      });
    } else {
      err(s, "only 'local' declarations and function definitions are allowed at top level " +
             "(put runtime statements in function _init())");
    }
  }

  // callback contract
  for (const cb of CALLBACKS) {
    const f = functions.get(cb);
    if (f && f.params.length > 0) err(f.node, `${cb}() takes no parameters`);
  }
  if (!functions.has("_update") && !functions.has("_update60") && !functions.has("_draw")) {
    diagnostics.push({
      file, line: 1, col: 1, severity: "error",
      message: "define _update60() (60fps) or _update() (30fps), and _draw() - the PICO-8 callback contract",
    });
  }
  if (functions.has("_update") && functions.has("_update60")) {
    err(functions.get("_update").node, "define _update() OR _update60(), not both");
  }

  // ---- kind inference to fixpoint, then a reporting pass --------------------
  function widen(slot, kind) {
    if (kind === "fixed" && slot.kind !== "fixed") { slot.kind = "fixed"; changed = true; }
  }

  function checkFunctionBodies() {
    for (const [, fn] of functions) checkFunction(fn);
  }

  reporting = false;
  for (let iter = 0; iter < 20; iter++) {
    changed = false;
    checkFunctionBodies();
    if (!changed) break;
  }
  reporting = true;
  checkFunctionBodies();

  function checkFunction(fn) {
    const scopes = [new Map()];
    fn.params.forEach((p, i) => {
      if (scopes[0].has(p)) err(fn.node, `duplicate parameter '${p}'`);
      scopes[0].set(p, { param: fn, paramIndex: i, get kind() { return fn.paramKinds[i]; } });
    });
    // stable local slots across fixpoint iterations: keyed on the AST decl node
    fn.localSlots = fn.localSlots || new Map();

    let loopDepth = 0;

    function slotFor(node, name) {
      const key = node; // decl AST node identity
      if (!fn.localSlots.has(key)) fn.localSlots.set(key, { kind: "int" });
      return fn.localSlots.get(key);
    }

    function declare(node, name, slot) {
      const top = scopes[scopes.length - 1];
      if (top.has(name)) err(node, `'${name}' is already declared in this scope`);
      top.set(name, slot);
    }

    function lookup(name) {
      for (let i = scopes.length - 1; i >= 0; i--) {
        if (scopes[i].has(name)) return scopes[i].get(name);
      }
      if (globals.has(name)) return globals.get(name);
      return null;
    }

    function widenSlot(sym, kind) {
      if (kind !== "fixed") return;
      if (sym.param) {
        if (sym.param.paramKinds[sym.paramIndex] !== "fixed") {
          sym.param.paramKinds[sym.paramIndex] = "fixed";
          changed = true;
        }
      } else if (sym.kind !== "fixed") {
        sym.kind = "fixed";
        changed = true;
      }
    }

    function checkBlock(b) {
      scopes.push(new Map());
      for (const s of b.stmts) checkStmt(s);
      scopes.pop();
    }

    function checkStmt(s) {
      switch (s.kind) {
        case "local": {
          s.slots = s.slots || [];
          s.names.forEach((name, idx) => {
            const init = s.inits[idx] ?? null;
            let kind = "int";
            if (init && init.kind === "nil") {
              init.okSentinel = true;   // local x = nil -> int holding the sentinel
            } else if (init) {
              const t = typeOf(init);
              if (t === "bool") err(init, "cannot store a boolean in a number variable");
              kind = t === "fixed" ? "fixed" : "int";
            }
            const slot = slotFor(s, name + "#" + idx);
            widen(slot, kind);
            s.slots[idx] = slot;
            declare(s, name, slot);
          });
          break;
        }
        case "assign": {
          if (s.target.kind === "member") {
            // a `p.field = ...` write to a pool element auto-declares the field
            // if it wasn't seen in an add(pool, {...}) literal (PICO-8 carts set
            // fields freely; the pool learns them from any write).
            if (s.target.object.kind === "name") {
              const osym = lookup(s.target.object.name);
              if (osym && osym.poolBinding && !osym.poolBinding.fields.has(s.target.field)) {
                osym.poolBinding.fields.set(s.target.field, { kind: "int" });
                changed = true;
              }
            }
            const mt = typeOf(s.target);   // annotates poolField or errors
            if (s.value.kind === "nil") s.value.okSentinel = true;   // field = nil -> sentinel
            const vt = typeOf(s.value);
            if (vt === "bool") { err(s.value, "pool fields are numbers"); break; }
            if (s.target.poolField) {
              const fl = s.target.poolField.pool.fields.get(s.target.poolField.field);
              let rk = vt;
              if (s.op === "/=") rk = "fixed";
              if (s.op !== "=" && s.op !== "\\=") rk = join(rk, fl.kind);
              if (rk === "fixed" && fl.kind !== "fixed") {
                if (fl.forceByte) err(s, `pool field '${s.target.poolField.field}' is declared byte-wide but assigned a fixed-point value`);
                fl.kind = "fixed"; changed = true;
              }
              // byte evidence: this + the add() literals are the ONLY store
              // paths (the parser rejects member targets in multi-assign).
              // Any compound op or non-constant / out-of-range value
              // disqualifies the field from u8 storage.
              {
                const cvv = s.op === "=" ? constEval(s.value) : null;
                if (!(Number.isInteger(cvv) && cvv >= 0 && cvv <= 255)) fl.notByte = true;
              }
              s.targetKind = fl.kind;
            } else {
              s.targetKind = mt;
            }
            if (s.op === "\\=" || s.op === "%=") {
              const d = constEval(s.value);
              s.divConst = d !== null && isPow2(d) ? d : null;
            }
            break;
          }
          if (s.target.kind === "index") {
            // element store: a[i] = v (or compound)
            const arr = arrayOf(s.target);
            if (!arr) break;
            const it = typeOf(s.target.index);
            if (it === "bool") err(s.target.index, "array index must be a number");
            const vt = typeOf(s.value);
            if (vt === "bool") { err(s.value, "cannot store a boolean in a number array"); break; }
            let rk = vt;
            if (s.op === "/=") rk = "fixed";
            if (s.op !== "=" && s.op !== "\\=") rk = join(rk, arr.elemKind);
            if (rk === "fixed" && arr.elemBytes) {
              err(s.value, "array8 elements are bytes 0-255 - flr() the value " +
                           "or use array() for fractional elements");
              break;
            }
            if (rk === "fixed" && arr.elemKind !== "fixed") { arr.elemKind = "fixed"; changed = true; }
            s.targetKind = arr.elemKind;
            s.valueKind = vt;
            if (s.op === "\\=" || s.op === "%=") {
              const d = constEval(s.value);
              s.divConst = d !== null && isPow2(d) ? d : null;
            }
            typeOf(s.target); // annotate target node kinds for the emitter
            break;
          }
          if (s.target.kind !== "name") {
            err(s, "cannot assign to this expression");
            break;
          }
          const sym = lookup(s.target.name);
          if (!sym) {
            err(s, `'${s.target.name}' is not declared - gbalua has no implicit globals; ` +
                   `declare it with 'local ${s.target.name} = ...'`);
            break;
          }
          // x = nil: store the reserved sentinel. Only a plain '=' (not +=/etc.)
          // makes sense; the variable stays a number.
          if (s.value.kind === "nil") {
            if (s.op !== "=") err(s, "nil can only be plain-assigned (x = nil), not with a compound operator");
            s.value.okSentinel = true;
            s.assignNil = true;
            s.targetKind = symKind(sym) === "fixed" ? "fixed" : "int";
            break;
          }
          const vt = typeOf(s.value);
          if (vt === "bool") { err(s.value, "cannot assign a boolean to a number variable"); break; }
          if (s.op === "..=") { err(s, "string concatenation is not supported yet"); break; }
          let resultKind = vt;
          if (s.op === "/=") resultKind = "fixed";
          if (s.op === "\\=") resultKind = "int";
          if (s.op !== "=") resultKind = join(resultKind, symKind(sym));
          if (s.op === "\\=") resultKind = "int";
          widenSlot(sym, resultKind);
          s.targetKind = symKind(sym);
          s.valueKind = vt;
          if (s.op === "\\=" || s.op === "%=") {
            const d = constEval(s.value);
            s.divConst = d !== null && isPow2(d) ? d : null;
          }
          break;
        }
        case "multiassign": {
          s.targetSyms = [];
          let kinds;
          if (s.fromCall) {
            // a, b, c = f(...) - the call must return at least as many values
            typeOf(s.values[0]);            // type/annotate the call
            const callee = s.values[0].callee;
            const fn2 = callee?.kind === "name" ? functions.get(callee.name) : null;
            const retCount = fn2?.retCount ?? (fn2?.hasReturnValue ? 1 : 0);
            if (!fn2) err(s, "multiple assignment from a call needs a user function that returns multiple values");
            else if (retCount < s.targets.length) {
              err(s, `'${callee.name}' returns ${retCount} value${retCount === 1 ? "" : "s"} but ${s.targets.length} are assigned`);
            }
            kinds = s.targets.map((_, i) => (fn2?.retKinds?.[i]) ?? (i === 0 ? (fn2?.retKind ?? "int") : "int"));
          } else {
            kinds = s.values.map((v) => {
              const t = typeOf(v);
              if (t === "bool") err(v, "cannot assign a boolean to a number variable");
              return t;
            });
          }
          s.targets.forEach((t2, i) => {
            if (t2.kind === "name") {
              const sym = lookup(t2.name);
              if (!sym) {
                err(s, `'${t2.name}' is not declared - declare it with 'local ${t2.name} = ...'`);
                return;
              }
              widenSlot(sym, kinds[i] ?? "int");
              s.targetSyms[i] = sym;
            } else {
              // struct field (o.x) or element (a[i]) target: type it so the
              // poolField/arraySym annotations reach the emitter, and widen the
              // field/element kind to the value kind (same as single assignment).
              const mt = typeOf(t2);
              if (mt === "bool") err(t2, "pool fields are numbers");
              if (t2.poolField && kinds[i] === "fixed") {
                const fl = t2.poolField.pool.fields.get(t2.poolField.field);
                if (fl && fl.kind !== "fixed") { fl.kind = "fixed"; changed = true; }
              }
            }
          });
          s.valueKinds = kinds;
          s.targetKinds = s.targets.map((t2, i) =>
            t2.kind === "name"
              ? (s.targetSyms[i] ? symKind(s.targetSyms[i]) : "int")
              : (t2.poolField ? t2.poolField.pool.fields.get(t2.poolField.field)?.kind ?? "int"
                 : t2.arraySym?.elemKind ?? "int"));
          break;
        }
        case "callstmt": callType(s.call, true); break;
        case "if": {
          for (const cl of s.clauses) { condType(cl.cond); checkBlock(cl.body); }
          if (s.elseBody) checkBlock(s.elseBody);
          break;
        }
        case "while": {
          condType(s.cond);
          loopDepth++; checkBlock(s.body); loopDepth--;
          break;
        }
        case "repeat": {
          loopDepth++;
          scopes.push(new Map());
          for (const st of s.body.stmts) checkStmt(st);
          condType(s.cond);
          scopes.pop();
          loopDepth--;
          break;
        }
        case "fornum": {
          const fk = typeOf(s.from), tk = typeOf(s.to);
          let sk = "int";
          if (s.step) {
            sk = typeOf(s.step);
            const sv = constEval(s.step);
            if (sv === null) err(s.step, "for-loop step must be a nonzero constant");
            else if (sv === 0) err(s.step, "for-loop step cannot be 0");
            s.stepConst = sv;
          } else {
            s.stepConst = 1;
          }
          const loopKind = join(join(fk, tk), sk);
          const slot = slotFor(s, s.name);
          widen(slot, loopKind);
          s.slot = slot;
          scopes.push(new Map());
          scopes[scopes.length - 1].set(s.name, slot);
          loopDepth++;
          for (const st of s.body.stmts) checkStmt(st);
          loopDepth--;
          scopes.pop();
          break;
        }
        case "return": {
          if (s.values && s.values.length > 1) {
            // multiple return: return a, b, c. The 1st value returns normally;
            // the rest go through reserved output slots (see emit). Track the
            // count and per-slot kinds so callers and the emitter agree.
            fn.hasReturnValue = true;
            fn.retCount = Math.max(fn.retCount ?? 1, s.values.length);
            fn.retKinds = fn.retKinds ?? [];
            s.valueKinds = [];
            s.values.forEach((v, i) => {
              const t = typeOf(v);
              if (t === "bool") err(v, "returning booleans is not supported yet; return 0/1 or restructure");
              if (t === "str") err(v, "returning strings is not supported");
              s.valueKinds[i] = t;
              if (t === "fixed") {
                if (i === 0 && fn.retKind !== "fixed") { fn.retKind = "fixed"; changed = true; }
                if (fn.retKinds[i] !== "fixed") { fn.retKinds[i] = "fixed"; changed = true; }
              } else if (!fn.retKinds[i]) { fn.retKinds[i] = "int"; }
            });
            break;
          }
          if (s.value) {
            const t = typeOf(s.value);
            if (t === "bool") err(s.value, "returning booleans is not supported yet; return 0/1 or restructure");
            fn.hasReturnValue = true;
            if (t === "fixed" && fn.retKind !== "fixed") { fn.retKind = "fixed"; changed = true; }
            s.valueKind = t;
          }
          break;
        }
        case "break": {
          if (loopDepth === 0) err(s, "'break' outside of a loop");
          break;
        }
        case "forall": {
          const pl = poolOf(s.pool, "all()");
          if (!pl) break;
          s.poolSym = pl;
          const binding = { poolBinding: pl, forall: s, kind: "int" };
          s.binding = binding;
          scopes.push(new Map());
          scopes[scopes.length - 1].set(s.name, binding);
          loopDepth++;
          for (const st of s.body.stmts) checkStmt(st);
          loopDepth--;
          scopes.pop();
          break;
        }
        case "do": checkBlock(s.body); break;
        case "function":
          err(s, "functions cannot be defined inside functions (no closures); move it to top level");
          break;
        default: break;
      }
    }

    function poolOf(expr2, what) {
      if (expr2.kind === "name") {
        const g = globals.get(expr2.name);
        if (g && g.kind === "pool") { expr2.poolSym = g; return g; }
      }
      err(expr2, `${what} needs a top-level pool ('local bullets = pool(8)')`);
      return null;
    }

    function addDelType(call, which, asStatement) {
      if (!asStatement) {
        err(call, `${which}() is a statement, not a value`);
      }
      if (call.args.length !== 2) {
        err(call, `${which}(pool, ${which === "add" ? "{...}" : "element"}) takes 2 arguments`);
        return "void";
      }
      const pl = poolOf(call.args[0], which + "()");
      if (!pl) return "void";
      if (which === "add") {
        const t = call.args[1];
        if (t.kind !== "table") {
          err(call, "add() takes a table literal: add(bullets, {x=1, y=2})");
          return "void";
        }
        if (pl.fields.size === 0) {
          for (const f of t.fields) {
            pl.fields.set(f.name, { kind: "int",
              forceByte: pl.byteFields ? pl.byteFields.has(f.name) : false });
          }
          if (pl.byteFields) {
            for (const bf of pl.byteFields) {
              if (!pl.fields.has(bf)) {
                err(call, `pool byte field '${bf}' is not a field of this pool`);
              }
            }
          }
        }
        const seen = new Set();
        for (const f of t.fields) {
          const fk = typeOf(f.expr);
          if (fk === "bool") err(f.expr, "pool fields are numbers (store 0/1)");
          let slot = pl.fields.get(f.name);
          if (!slot) {
            // union model: an add() field not yet on the pool registers it (the
            // pool holds every field any add() or p.field write uses; omitted
            // fields just default to 0).
            slot = { kind: "int" };
            pl.fields.set(f.name, slot);
            changed = true;
          }
          {
            const cvv = constEval(f.expr);
            if (!(Number.isInteger(cvv) && cvv >= 0 && cvv <= 255)) slot.notByte = true;
          }
          if (fk === "fixed" && slot.kind !== "fixed") {
            slot.kind = "fixed";
            changed = true;
          }
          seen.add(f.name);
        }
        /* fields the pool has but this add() omits default to 0 - no error */
        call.poolSym = pl;
        return "void";
      }
      // del(pool, e): e must be the all()-loop binding for that pool
      const e2 = call.args[1];
      const sym = e2.kind === "name" ? lookup(e2.name) : null;
      if (!sym || !sym.poolBinding || sym.poolBinding !== pl) {
        err(call, "del(pool, e) needs the loop variable of 'for e in all(pool)'");
        return "void";
      }
      call.poolSym = pl;
      call.bindingSym = sym;
      return "void";
    }

    function symKind(sym) {
      if (sym.param) return sym.param.paramKinds[sym.paramIndex];
      return sym.kind;
    }

    // resolve `name[...]` to its top-level array symbol (or error)
    function arrayOf(indexNode) {
      const obj = indexNode.object;
      if (obj.kind !== "name") {
        err(indexNode, "only top-level arrays can be indexed");
        return null;
      }
      const sym = globals.get(obj.name);
      if (!sym || sym.kind !== "array") {
        err(indexNode, `'${obj.name}' is not an array - declare one at top level with ` +
                       `'local ${obj.name} = array(16)'`);
        return null;
      }
      indexNode.arraySym = sym;
      return sym;
    }

    function condType(e) {
      const t = typeOf(e);
      if (t !== "bool") {
        err(e, "conditions must be boolean - PICO-8 Lua treats 0 as true but compiled C does not, " +
               "so gbalua requires an explicit comparison (write 'x ~= 0' or 'x > 0')");
      }
    }

    // returns "int" | "fixed" | "bool" | "void"
    function callType(call, asStatement = false) {
      const callee = call.callee;

      // gt.* was the GameTank-only namespace; the GBA has no such escape hatch
      // (its hardware is first-class verbs). Reject it with a pointer to the docs.
      if (callee.kind === "member" && callee.object.kind === "name" && callee.object.name === "gt") {
        err(call, `'gt.${callee.field}' is a GameTank-only verb and isn't available on the GBA - use the GBA verbs instead (see docs/CHEATSHEET.md)`);
        return "int";
      }

      if (callee.kind !== "name") {
        err(call, "only plain function calls are supported");
        return "int";
      }

      // builtins - but a user-defined function of the same name WINS (a cart may
      // define its own map/mget/etc.; the builtin must not shadow it).
      const b = functions.has(callee.name) ? undefined : BUILTINS[callee.name];
      if (b && b.special === "array") {
        err(call, "array(n) is only allowed as a top-level initializer: 'local pool = array(16)'");
        return "int";
      }
      if (b && b.special === "pool") {
        err(call, "pool(n) is only allowed as a top-level initializer: 'local bullets = pool(8)'");
        return "int";
      }
      if (b && b.special === "print") {
        call.sig = b;
        if (call.args.length < 1 || call.args.length > 4) {
          err(call, "print takes 1-4 arguments: print(v), print(v,c), or print(v,x,y,[c])");
        }
        // 1-2 args = cursor form (no x,y): print(v) or print(v, color)
        call.cursorForm = call.args.length <= 2;
        if (call.args[0] && call.args[0].kind === "string") call.args[0].inPrint = true;
        const t0 = call.args[0] ? typeOf(call.args[0]) : "int";
        call.printKind = t0 === "str" ? "str" : "num";
        if (t0 === "bool") err(call.args[0], "cannot print a boolean");
        if (call.cursorForm) {
          // the optional 2nd arg is the COLOR
          if (call.args[1]) { const tc = typeOf(call.args[1]); if (tc === "bool" || tc === "str") err(call.args[1], "print color must be a number"); }
          return "int";
        }
        call.args.slice(1).forEach((a) => {
          const t = typeOf(a);
          if (t === "bool" || t === "str") err(a, "print coordinates/color must be numbers");
        });
        return "int";
      }
      if (b && (b.special === "add" || b.special === "del")) {
        call.sig = b;
        return addDelType(call, b.special, asStatement);
      }
      // rnd({a,b,c}) - PICO-8 "pick a random element". The list must be constant;
      // we materialize it as a hidden top-level const array and emit a random
      // index into it. (rnd of a plain number stays the normal builtin below.)
      if (callee.name === "rnd" && call.args.length === 1 && call.args[0].kind === "arraytable") {
        if (call.rndList) return call.rndList.kind;   // already built (fixpoint re-pass)
        const el = call.args[0].elements;
        const vals = [];
        let anyFixed = false, bad = null;
        for (const e2 of el) {
          const v = constEval(e2);
          if (v === null) { bad = e2; break; }
          if (!Number.isInteger(v)) anyFixed = true;
          vals.push(v);
        }
        if (el.length === 0) { err(call, "rnd({}) has no elements to pick from"); return "int"; }
        if (bad) { err(bad, "rnd({...}) needs a constant list of numbers"); return "int"; }
        const nm = `__rndlist${rndListSeq++}`;
        const allByte = !anyFixed && vals.every((v) => v >= 0 && v <= 255);
        globals.set(nm, {
          kind: "array", elemKind: anyFixed ? "fixed" : "int", elemBytes: allByte,
          size: vals.length, initVal: 0, initList: vals, node: call,
        });
        call.rndList = { name: nm, len: vals.length, kind: anyFixed ? "fixed" : "int" };
        return anyFixed ? "fixed" : "int";
      }
      if (b) {
        if (b.audio) { usesAudio.flag = true; usesMusic.flag = true; }
        checkArgs(call, b.params, callee.name);
        call.sig = b;
        // a "fn" (callback) arg is a bare function NAME, not a value - don't run
        // typeOf on it (that would trip the "functions are not values" error);
        // its argKind is a handle. Same skip for pool/array/str name-args.
        call.argKinds = call.args.map((a, i) => {
          const p = b.params[i];
          if (a.callbackRef || (p && (p[0] === "fn" || p[0] === "pool" ||
              p[0] === "array" || p[0] === "array8"))) return "int";
          // "optr" is an opaque pointer HANDLE carried as an int (a Sprite*,
          // sample blob, ...); type it as int, don't reject the value.
          if (p && p[0] === "optr") { typeOf(a); return "int"; }
          return typeOf(a);
        });
        if (b.ret === "same") return call.argKinds.some((k) => k === "fixed") ? "fixed" : "int";
        return b.ret;
      }

      // user functions
      const fn2 = functions.get(callee.name);
      if (!fn2) {
        err(call, `'${callee.name}' is not a function`);
        return "int";
      }
      if (CALLBACKS.includes(callee.name)) {
        err(call, `${callee.name}() is called by the runtime; do not call it yourself`);
      }
      if (call.args.length !== fn2.params.length) {
        err(call, `${callee.name}() takes ${fn2.params.length} argument(s), got ${call.args.length}`);
      }
      call.args.forEach((a, i) => {
        const t = typeOf(a);
        if (t === "bool") err(a, "cannot pass a boolean as a number argument");
        if (i < fn2.paramKinds.length && t === "fixed" && fn2.paramKinds[i] !== "fixed") {
          fn2.paramKinds[i] = "fixed";
          changed = true;
        }
      });
      call.userFn = fn2;
      if (!asStatement && !fn2.hasReturnValue) {
        err(call, `${callee.name}() returns nothing and cannot be used in an expression`);
      }
      return fn2.hasReturnValue ? fn2.retKind : "void";
    }

    function checkArgs(call, params, name) {
      const required = params.filter(([, opt]) => !opt).length;
      if (call.args.length < required || call.args.length > params.length) {
        err(call, `${name}() takes ${required === params.length ? required : `${required}-${params.length}`} argument(s), got ${call.args.length}`);
      }
      call.args.forEach((a, i) => {
        // an "array" param wants a bare array-global name passed by pointer -
        // arrays aren't values, so don't type-check it as a number.
        if (params[i] && params[i][0] === "pool") {
          const sym = a.kind === "name" ? lookup(a.name) : null;
          if (!sym || sym.kind !== "pool") {
            err(a, `${name}() argument ${i + 1} must be a pool`);
          } else {
            const bare = name.startsWith("gt.") ? name.slice(3) : name;
            if (bare === "parts_step") {
              for (const f of ["x", "y", "vx", "vy"]) {
                const fl = sym.fields.get(f);
                if (!fl) err(a, `${name}() pool needs a fixed field '${f}'`);
                else if (fl.kind !== "fixed") err(a, `${name}() pool field '${f}' must be fixed`);
              }
              a.sym = sym;
              return;
            }
            const need = bare === "pool_sprs" ? ["x", "y"] : ["x", "y", "sx", "sy"];
            for (const f of need) {
              const fl = sym.fields.get(f);
              if (!fl) err(a, `${name}() pool needs an int field '${f}'`);
              else if (fl.kind !== "int" || fl.forceByte) err(a, `${name}() pool field '${f}' must be a 16-bit int`);
            }
            a.sym = sym;
          }
          return;
        }
        if (params[i] && params[i][0] === "str") {
          if (a.kind !== "string") err(a, `${name}() argument ${i + 1} must be a string literal`);
          else a.inPrint = true;   // string literal in an allowed position (not just print)
          return;
        }
        // a "fn" param is a CALLBACK: a bare reference to a top-level function.
        // Flat ROM makes an indirect call safe (no bank to unmap), and the call
        // graph stays complete because the reference is static. The emitter
        // renders it as &gtl_<name>. This is what unlocks SGDK's callback API
        // (task.h, SYS_setVIntCallback, sprite frame-change hooks, ...).
        if (params[i] && params[i][0] === "fn") {
          if (a.kind !== "name" || !functions.has(a.name)) {
            err(a, `${name}() argument ${i + 1} must be a top-level function name (a callback)`);
          } else {
            a.callbackRef = a.name;        // annotate for the emitter
            functions.get(a.name).addressTaken = true;  // keep it (never dead-code it)
          }
          return;
        }
        if (params[i] && (params[i][0] === "array" || params[i][0] === "array8")) {
          const want8 = params[i][0] === "array8";
          const sym = a.kind === "name" ? lookup(a.name) : null;
          if (!sym || sym.kind !== "array") {
            err(a, `${name}() argument ${i + 1} must be an array (declared with ${want8 ? "array8(n)" : "array(n)"})`);
          } else if (!want8 && sym.elemBytes) {
            err(a, `${name}() argument ${i + 1} must be a 16-bit array(n) - array8 ` +
                   `elements are single bytes and the runtime reads int pairs`);
          } else if (want8 && !sym.elemBytes) {
            err(a, `${name}() argument ${i + 1} must be an array8(n) - this runtime reads single bytes`);
          } else {
            a.sym = sym;   // annotate for the emitter
          }
          return;
        }
        // a "flip" param (spr flip_x/flip_y) is a truthy flag - bool is the
        // natural value here, so don't reject it the way number args do.
        if (params[i] && params[i][0] === "flip") { typeOf(a); return; }
        const t = typeOf(a);
        if (t === "bool") err(a, `cannot pass a boolean as a number argument to ${name}()`);
      });
    }

    function typeOf(e) {
      const t = typeOfInner(e);
      e.tk = t; // annotate for the emitter
      return t;
    }

    function typeOfInner(e) {
      switch (e.kind) {
        case "number": return e.isInt ? "int" : "fixed";
        case "bool": return "bool";
        case "name": {
          const sym = lookup(e.name);
          if (!sym) {
            if (functions.has(e.name)) {
              err(e, `'${e.name}' is a function - functions are not values in gbalua (no closures); call it`);
            } else if (BUILTINS[e.name]) {
              err(e, `'${e.name}' is a builtin function - call it: ${e.name}(...)`);
            } else if (e.name === "gt") {
              err(e, "'gt' is the hardware module; use gt.<function>(...)");
            } else {
              err(e, `'${e.name}' is not declared`);
            }
            return "int";
          }
          e.sym = sym;
          return symKind(sym);
        }
        case "member": {
          if (e.object.kind === "name" && e.object.name === "gt") {
            err(e, `'gt.${e.field}' is a GameTank-only verb and isn't available on the GBA (see docs/CHEATSHEET.md)`);
            return "int";
          }
          if (e.object.kind === "name") {
            const sym = lookup(e.object.name);
            if (sym && sym.poolBinding) {
              let fl = sym.poolBinding.fields.get(e.field);
              if (!fl) {
                // auto-declare an unseen field (PICO-8 carts read/write pool
                // fields freely; the pool takes the union of all fields used).
                fl = { kind: "int" };
                sym.poolBinding.fields.set(e.field, fl);
                changed = true;
              }
              e.poolField = { pool: sym.poolBinding, field: e.field, forall: sym.forall };
              return fl.kind;
            }
          }
          err(e, "field access is not supported yet outside 'for e in all(pool)' loops");
          return "int";
        }
        case "index": {
          const arr = arrayOf(e);
          if (!arr) return "int";
          const it = typeOf(e.index);
          if (it === "bool") err(e.index, "array index must be a number");
          return arr.elemKind;
        }
        case "len": {
          if (e.expr.kind === "name") {
            const sym = globals.get(e.expr.name);
            if (sym && sym.kind === "array") { e.arraySym = sym; return "int"; }
            if (sym && sym.kind === "pool") { e.poolSym = sym; return "int"; }
          }
          err(e, "'#' works on top-level arrays and pools only");
          return "int";
        }
        case "call": return callType(e);
        case "neg": {
          const t = typeOf(e.expr);
          if (t === "bool") err(e, "cannot negate a boolean");
          return t;
        }
        case "bnot": {
          // PICO-8 ~x flips all 32 bits including the fraction -> always fixed
          const t = typeOf(e.expr);
          if (t === "bool") err(e, "'~' needs a number");
          return "fixed";
        }
        case "not": {
          const t = typeOf(e.expr);
          if (t !== "bool") err(e, "'not' needs a boolean; write an explicit comparison");
          return "bool";
        }
        case "table":
          err(e, "table literals are only allowed inside add(pool, {...})");
          return "int";
        case "arraytable":
          err(e, "an array table {1, 2, 3} is only allowed as a top-level constant " +
                 "('local levels = {1, 2, 3}'); it becomes a fixed read-only C array");
          return "int";
        case "string":
          if (!e.inPrint) err(e, "strings can only be used in print() for now");
          return "str";
        case "nil":
          // nil reaching typeOf means it's used as a value somewhere other than
          // the sentinel positions (x = nil, x ==/~= nil), which are special-
          // cased in the assignment and comparison checks before typeOf runs.
          if (!e.okSentinel) {
            err(e, "nil is only supported as an empty-marker: 'x = nil', 'x == nil', 'x ~= nil'. " +
                   "It has no value, so it can't be used in arithmetic, returned, or passed as an argument");
          }
          return "int";
        case "binop": return binopType(e);
        default: return "int";
      }
    }

    function binopType(e) {
      const { op } = e;
      if (op === "and" || op === "or") {
        const lt = typeOf(e.left), rt = typeOf(e.right);
        if (lt !== "bool" || rt !== "bool") {
          err(e, `'${op}' needs boolean operands (PICO-8's 'x or default' value idiom needs nil, which gbalua doesn't have)`);
        }
        return "bool";
      }
      if (["<", ">", "<=", ">="].includes(op)) {
        const lt = typeOf(e.left), rt = typeOf(e.right);
        if (lt === "bool" || rt === "bool") err(e, "cannot compare booleans with <");
        e.cmpKind = join(lt === "bool" ? "int" : lt, rt === "bool" ? "int" : rt);
        return "bool";
      }
      if (op === "==" || op === "~=") {
        // nil comparison: x == nil / x ~= nil -> compare the variable against the
        // reserved sentinel. Mark the nil operand OK so typeOf doesn't reject it.
        if (e.left.kind === "nil" || e.right.kind === "nil") {
          const nilSide = e.left.kind === "nil" ? e.left : e.right;
          const other = e.left.kind === "nil" ? e.right : e.left;
          nilSide.okSentinel = true;
          const ot = typeOf(other);
          if (ot === "bool" || ot === "str") err(e, "only a number variable can be compared to nil");
          e.cmpKind = "int";
          e.nilCompare = true;
          return "bool";
        }
        const lt = typeOf(e.left), rt = typeOf(e.right);
        if ((lt === "bool") !== (rt === "bool")) err(e, "cannot compare a number with a boolean");
        e.cmpKind = lt === "bool" ? "bool" : join(lt, rt);
        return "bool";
      }
      if (op === "..") {
        err(e, "string concatenation is not supported yet");
        return "int";
      }

      const lt = typeOf(e.left), rt = typeOf(e.right);
      if (lt === "bool" || rt === "bool") {
        err(e, `'${op}' needs number operands`);
        return "int";
      }

      if (op === "/") {
        const d = constEval(e.right);
        e.divConst = d !== null && isPow2(d) ? d : null;
        if (d === 0) warn(e, "division by constant zero saturates to ±32767.99998 (PICO-8 semantics)");
        return "fixed";
      }
      if (op === "\\") {
        const d = constEval(e.right);
        e.divConst = d !== null && isPow2(d) ? d : null;
        e.operandKind = join(lt, rt);
        return "int";
      }
      if (op === "%") {
        const d = constEval(e.right);
        e.divConst = d !== null && isPow2(d) ? d : null;
        return join(lt, rt);
      }
      if (["&", "|", "^^", "<<", ">>", ">>>"].includes(op)) {
        return join(lt, rt);
      }
      // + - *
      return join(lt, rt);
    }

    checkBlock(fn.node.body);
  }

  return { diagnostics, symbols: { globals, functions, usesAudio: usesAudio.flag, usesMusic: usesMusic.flag } };
}
